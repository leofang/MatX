////////////////////////////////////////////////////////////////////////////////
// BSD 3-Clause License
//
// Copyright (c) 2021, NVIDIA Corporation
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its
//    contributors may be used to endorse or promote products derived from
//    this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
/////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <algorithm>
#include <cassert>
#include <cuda/std/tuple>
#include <initializer_list>
#include "matx_exec_kernel.h"
#include "matx_scalar_ops.h"
#include "matx_tensor.h"
#include "matx_executor.h"
#include "matx_transpose.cuh"
#include "matx_type_utils.h"

namespace matx
{

/**
 * @brief Utility operator for multiplying scalars by a complex value
 * 
 * @tparam T Complex type
 * @tparam S Scalar type
 * @param n Scalar value
 * @param c Complex value
 * @return Product result
 */
template <typename T, typename S>
__MATX_INLINE__
    typename std::enable_if_t<!std::is_same_v<T, S> && std::is_arithmetic_v<S>,
                              cuda::std::complex<T>>
        __MATX_HOST__ __MATX_DEVICE__ operator*(const cuda::std::complex<T> &c, S n)
{
  return c * T(n);
}

/**
 * @brief Utility operator for multiplying scalars by a complex value
 * 
 * @tparam T Complex type
 * @tparam S Scalar type
 * @param n Scalar value
 * @param c Complex value
 * @return Product result
 */
template <typename T, typename S>
__MATX_INLINE__
    typename std::enable_if_t<!std::is_same_v<T, S> && std::is_arithmetic_v<S>,
                              cuda::std::complex<T>>
        __MATX_HOST__ __MATX_DEVICE__ operator*(S n, const cuda::std::complex<T> &c)
{
  return T(n) * c;
}


/**
 * Concatenate operators
 *
 * Class for concatening operators along a single dimension. Sizes of the operators not
 * being concatenated must be the same, and the new operator has dimensions equal to the original
 * operator on non-index dimension, and the sum of sizes along the index dimension.
 */
  namespace detail {  
  template <int Dim, typename... Ts>
  class Concatenate : public BaseOp<Concatenate<Dim, Ts...>>
  {
    using first_type = std::tuple_element_t<0, std::tuple<Ts...>>;
    using first_value_type = typename first_type::value_type;
    static constexpr int RANK = first_type::Rank();

  public:
    // Scalar type of operation
    using scalar_type = first_value_type;

    __MATX_INLINE__ Concatenate(Ts... ts) : ops_(ts...)
    {
      static_assert(RANK > 0, "Cannot concatenate rank-0 tensors");
      static_assert(sizeof...(Ts) > 0, "Must have more than one tensor to concatenate");
      static_assert((... && (RANK == ts.Rank())));

      auto tsum = [&](int d, auto ...args){ return (args.Size(d) + ...); };
      for (int32_t i = 0; i < RANK; i++) {
        size_[i] = (i == Dim) ? tsum(i, ts...) : pp_get<0>(ts...).Size(i);
      }
    }  

    // Base case. Cannot be reached
    template <size_t I = 0, typename... Is, std::enable_if_t<I == sizeof...(Ts), bool> = true>
    __MATX_INLINE__ __MATX_DEVICE__ __MATX_HOST__ auto GetVal(cuda::std::tuple<Is...> tup) const {
      return static_cast<first_value_type>(0);
    }

    /* Check if the value of the index we're concatenating is smaller the size of the current
       operator size in that dimension. If so, we're on the correct operator and just return
       operator() from it. Otherwise we recursively call the same function moving to another 
       operator with a smaller index. */
    template <size_t I = 0, typename... Is, std::enable_if_t<I < sizeof...(Ts), bool> = true>
    __MATX_INLINE__ __MATX_DEVICE__ __MATX_HOST__ auto GetVal(cuda::std::tuple<Is...> tup) const
    {
      if (cuda::std::get<Dim>(tup) < cuda::std::get<I>(ops_).Size(Dim)) {
        return mapply([&](auto &&...args) -> first_value_type {
          return cuda::std::get<I>(ops_).operator()(args...);
        }, tup);
      }

      cuda::std::get<Dim>(tup) -= cuda::std::get<I>(ops_).Size(Dim);
      return static_cast<first_value_type>(GetVal<I + 1, Is...>(tup));
    }    

    template <typename... Is>
    __MATX_INLINE__ __MATX_DEVICE__ __MATX_HOST__ auto operator()(Is... is) const
    {
      return static_cast<first_value_type>(GetVal<0, Is...>(cuda::std::make_tuple(is...)));
    }
   
    
    static __MATX_INLINE__ constexpr __MATX_HOST__ __MATX_DEVICE__ int32_t Rank() noexcept
    {
      return RANK;
    }

    constexpr index_t __MATX_INLINE__ __MATX_HOST__ __MATX_DEVICE__ Size(int dim) const noexcept
    {
      return size_[dim];
    }

    private:
      cuda::std::tuple<Ts...> ops_;
      std::array<index_t, RANK> size_;    
  };
  }

  /**
   * @brief Concatenate multiple operators along a dimension
   * 
   * @tparam Dim dimension to concatenate
   * @tparam Ts operator types
   * @param ts operators
   * @return concatenated operator 
   */
  template <int Dim, typename... Ts>
  __MATX_INLINE__ __MATX_HOST__  auto concat(Ts... ts)
  {
    static_assert(((Dim < ts.Rank()) && ...), "Concatenation dimension larger than tensor rank");
    return detail::Concatenate<Dim, Ts...>{ts...};
  }  

/**
 * Chain multiple operator statements
 *
 * Takes a variable list of operator statements to execute concurrently.
 * Chaining may improve performance over executing each operation separately.
 */
  namespace detail {
  template<class Op1, class Op2>
  class CommaOp : public BaseOp<CommaOp<Op1, Op2>>{
      public:
        __MATX_DEVICE__ __MATX_HOST__ __MATX_INLINE__  CommaOp(Op1 op1, Op2 op2) : op1_(op1), op2_(op2) {
          MATX_STATIC_ASSERT_STR(Op1::Rank() == Op2::Rank(), matxInvalidSize, 
            "Chained expressions using the comma operator must match in rank");
        }

        template <typename... Is>
        auto __MATX_INLINE__ __MATX_HOST__ __MATX_DEVICE__ operator()(Is... indices) const {
          op1_(indices...);
          return op2_(indices...);
        }                       

        static __MATX_INLINE__ constexpr __MATX_HOST__ __MATX_DEVICE__ int32_t Rank() noexcept
        {
          return Op2::Rank();
        }

        constexpr __MATX_INLINE__ __MATX_HOST__ __MATX_DEVICE__ auto Size(int dim) const noexcept
        {
          return op2_.Size(dim);
        }        
    private:
        typename base_type<Op1>::type op1_;
        typename base_type<Op2>::type op2_;
  };  
  }

  /**
   * @brief Comma operator for evaluating multiple operators
   * 
   * @tparam T Left operator type
   * @tparam S Right operator type
   * @param l Left operator value
   * @param r Right operator value
   * @return Result of comma operator
   */
  template <typename T, typename S, std::enable_if_t<is_matx_op<T>() && is_matx_op<S>(), bool> = true>
  __MATX_INLINE__ __MATX_HOST__ __MATX_DEVICE__ auto operator,(T l, S r)
  {
    return detail::CommaOp(l, r);
  }

  /**
 * Make a deep copy of a view into another view
 *
 * Copies the data from a view into another view. Views should normally be
 * backed by different data objects, but it's not necessary if there is no
 * overlap between the soure and destination. If the source in destination
 * overlap in any way, it is a race condition and the result of the operation
 * is undefined.
 *
 * Both tensor views must be the same rank and size in every dimension
 *
 * @param out
 *   Tensor to copy into
 * @param in
 *   Tensor to copy from
 * @param stream
 *   CUDA stream to operate in
 */
  template <typename OutputTensor, typename InputTensor>
  __MATX_INLINE__ void copy(OutputTensor &out, const InputTensor &in,
                   const cudaStream_t stream)
  {
    for (int i = 0; i < OutputTensor::Rank(); i++)
    {
      MATX_ASSERT(out.Size(i) == in.Size(i), matxInvalidSize);
    }

    (out = in).run(stream);
  };

  /**
 * Transpose the outer dimensions of a tensor view out-of-place
 *
 * Transposes the two fastest-changing dimensions of a tensor. Any higher
 * dimension is untouched. This has the same effect as permute with {1,0} as the
 * last two dims, but it is much faster for tensors that are already contiguous.
 * For tensors that are not a contiguous view, this function is not allowed.
 *
 * Both tensor views must be the same rank, and the dimensions that moved must
 * match their original size
 *
 * @param out
 *   Tensor to copy into
 * @param in
 *   Tensor to copy from
 * @param stream
 *   CUDA stream to operate in
 *
 */
  template <typename OutputTensor, typename InputTensor>
  __MATX_INLINE__ void transpose(OutputTensor &out,
                        const InputTensor &in,
                        const cudaStream_t stream)
  {
    constexpr int RANK = OutputTensor::Rank();
    if constexpr (RANK <= 1)
    {
      return;
    }

    if (!in.IsContiguous())
    {
      MATX_THROW(matxInvalidSize, "Must have a linear tensor view for transpose");
    }

#ifdef __CUDACC__  
    size_t shm = sizeof(typename OutputTensor::scalar_type) * TILE_DIM * (TILE_DIM + 1);
    if constexpr (RANK == 2)
    {
      dim3 block(TILE_DIM, TILE_DIM);
      dim3 grid(static_cast<int>((in.Size(RANK - 1) + TILE_DIM - 1) / TILE_DIM),
                static_cast<int>((in.Size(RANK - 2) + TILE_DIM - 1) / TILE_DIM));
      transpose_kernel_oop<<<grid, block, shm, stream>>>(out, in);
    }
    else if constexpr (RANK >= 3)
    {
      int batch_dims =
          static_cast<int>(in.TotalSize() / (in.Size(RANK - 1) * in.Size(RANK - 2)));

      dim3 block(TILE_DIM, TILE_DIM);
      dim3 grid(static_cast<int>((in.Size(RANK - 1) + TILE_DIM - 1) / TILE_DIM),
                static_cast<int>((in.Size(RANK - 2) + TILE_DIM - 1) / TILE_DIM),
                batch_dims);
      transpose_kernel_oop<<<grid, block, shm, stream>>>(out, in);
    }
#endif    
  };

/**
 * Permute a tensor view out-of-place
 *
 * Rearranges the dimensions of a tensor view without touching the data. This is
 * accomplished by changing the strides between dimensions to reflect the new
 * transposed order. This function can result in very in efficient memory
 * accesses, so it's recommended only to use in places performance is not
 * critical.
 *
 * Both tensor views must be the same rank, and the dimensions that moved must
 * match their original size
 *
 * @param out
 *   Tensor to copy into
 * @param in
 *   Tensor to copy from
 * @param dims
 *   Order of transposed tensor dimensions
 * @param stream
 *   CUDA stream to operate in
 *
 */
  template <class T, int Rank>
  __MATX_INLINE__ void permute(detail::tensor_impl_t<T, Rank> &out, const detail::tensor_impl_t<T, Rank> &in,
                      const std::initializer_list<uint32_t> &dims,
                      const cudaStream_t stream)
  {
    // This is very naive, we should make optimized versions for various swizzles
    auto in_t = in.Permute(dims.begin());

    copy(out, in_t, stream);
  };


  /**
 * Conditionally execute an operator
 *
 * Compares two operators or views and conditionally executes the second
 * statement if the first is true. Values from an operator are executed
 * individually, and the only requirement for the conditional is the comparison
 * operator must be defined for the particular type. For example, operator< on
 * two integers is okay, but the same operator on two complex numbers will give
 * a compiler error.
 *
 */
  template <typename T1, typename T2>
  class IF : public BaseOp<IF<T1, T2>>
  {
  private:
    typename detail::base_type<T1>::type cond_;
    typename detail::base_type<T2>::type op_;
    std::array<index_t, detail::matx_max(detail::get_rank<T1>(), detail::get_rank<T2>())> size_;

  public:
    using scalar_type = void; ///< Scalar type for type extraction

    /**
     * @brief Constructor for an IF statement
     * 
     * @param cond Condition to perform the IF/ELSE on
     * @param op Operator if conditional branch is true
     */    
    __MATX_INLINE__ IF(T1 cond, T2 op) : cond_(cond), op_(op)
    {
      static_assert((!is_tensor_view_v<T2>),
                    "Only operator emmitters are allowed in IF. Tensor views are "
                    "not allowed");
      constexpr index_t rank1 = detail::get_rank<T1>();
      constexpr index_t rank2 = detail::get_rank<T2>();
      static_assert(rank1 == -1 || rank1 == Rank());
      static_assert(rank2 == -1 || rank2 == Rank());

      if constexpr (Rank() > 0)
      {
        for (int i = 0; i < Rank(); i++)
        {
          index_t size1 = detail::get_expanded_size<Rank()>(cond_, i);
          index_t size2 = detail::get_expanded_size<Rank()>(op_, i);
          size_[i] = detail::matx_max(size1, size2);          
          MATX_ASSERT(size1 == 0 || size1 == Size(i), matxInvalidSize);
          MATX_ASSERT(size2 == 0 || size2 == Size(i), matxInvalidSize);
        }
      }
    }

    /**
     * @brief Operator() for getting values of an if operator
     * 
     * @tparam Is Index types
     * @param indices Index values
     */
    template <typename... Is>
    auto __MATX_INLINE__ __MATX_HOST__ __MATX_DEVICE__ operator()(Is... indices) const {
      if (get_value(cond_, indices...)) {
        get_value(op_, indices...);
      }
    }   

    /**
     * @brief Rank of IF operator
     * 
     * @return Rank
     */    
    static __MATX_INLINE__ constexpr __MATX_HOST__ __MATX_DEVICE__ int32_t Rank()
    {
      return detail::matx_max(detail::get_rank<T1>(), detail::get_rank<T2>());
    }

    /**
     * @brief Size of dimension of operator
     * 
     * @param dim Dimension to get size of
     * @return Size of dimension 
     */    
    constexpr index_t __MATX_INLINE__ __MATX_HOST__ __MATX_DEVICE__ Size(int dim) const
    {
      return size_[dim];
    }
  };

  /**
 * Conditionally execute an operator, otherwise execute a different operator
 *
 * Compares two operators or views and conditionally executes the second
 * statement if the first is true, otherwise executes the third statement.
 * Values from an operator are executed individually, and the only requirement
 * for the conditional is the comparison operator must be defined for the
 * particular type. For example, operator< on two integers is okay, but the same
 * operator on two complex numbers will give a compiler error.
 *
 */
  template <typename C1, typename T1, typename T2>
  class IFELSE : public BaseOp<IFELSE<C1, T1, T2>>
  {
  private:
    typename detail::base_type<C1>::type cond_;
    typename detail::base_type<T1>::type op1_;
    typename detail::base_type<T2>::type op2_;    
    std::array<index_t, detail::matx_max(detail::get_rank<C1>(), detail::get_rank<T1>(), detail::get_rank<T2>())> size_;

  public:
    using scalar_type = void; ///< Scalar type for type extraction

    /**
     * @brief Constructor for an IFELSE statement
     * 
     * @param cond Condition to perform the IF/ELSE on
     * @param op1 Operator if conditional branch is true
     * @param op2 Operator if conditional branch is false
     */
    __MATX_INLINE__ IFELSE(C1 cond, T1 op1, T2 op2) : cond_(cond), op1_(op1), op2_(op2)
    {
      static_assert((!is_tensor_view_v<T1> && !is_tensor_view_v<T2>),
                    "Only operator emmitters are allowed in IFELSE. Tensor views "
                    "are not allowed");
      constexpr int32_t rank0 = detail::get_rank<C1>();
      constexpr int32_t rank1 = detail::get_rank<T1>();
      constexpr int32_t rank2 = detail::get_rank<T2>();
      static_assert(rank0 == -1 || rank0 == Rank());
      static_assert(rank1 == -1 || rank1 == Rank());
      static_assert(rank2 == -1 || rank2 == Rank());

      if constexpr (Rank() > 0)
      {
        for (int i = 0; i < Rank(); i++)
        {
          index_t size0 = detail::get_expanded_size<Rank()>(cond_, i);
          index_t size1 = detail::get_expanded_size<Rank()>(op1, i);
          index_t size2 = detail::get_expanded_size<Rank()>(op2, i);
          size_[i] = detail::matx_max(size0, size1, size2);
          MATX_ASSERT(size0 == 0 || size0 == Size(i), matxInvalidSize);
          MATX_ASSERT(size1 == 0 || size1 == Size(i), matxInvalidSize);
          MATX_ASSERT(size2 == 0 || size2 == Size(i), matxInvalidSize);          
        }
      }
    }

    /**
     * @brief Operator() for getting values of an if/else
     * 
     * @tparam Is Index types
     * @param indices Index values
     */
    template <typename... Is>
    auto __MATX_INLINE__ __MATX_HOST__ __MATX_DEVICE__ operator()(Is... indices) const {
      if (get_value(cond_, indices...)) {
        get_value(op1_, indices...);
      }
      else {
        get_value(op2_, indices...);
      }
    }      

    /**
     * @brief Rank of IF/ELSE operator
     * 
     * @return Rank
     */
    static __MATX_INLINE__ constexpr __MATX_HOST__ __MATX_DEVICE__ int32_t Rank()
    {
      return detail::matx_max(detail::get_rank<C1>(), detail::get_rank<T1>(), detail::get_rank<T2>());
    }

    /**
     * @brief Size of dimension of operator
     * 
     * @param dim Dimension to get size of
     * @return Size of dimension 
     */
    constexpr index_t __MATX_INLINE__ __MATX_HOST__ __MATX_DEVICE__ Size(int dim) const
    {
      return size_[dim];
    }
  };

/**
 * Selects elements of an operator given a list of indices in another operator
 *
 */
  namespace detail {
  template <typename T, typename IdxType>
  class SelectOp : public BaseOp<SelectOp<T, IdxType>>
  {
  private:
    typename base_type<T>::type op_;
    typename base_type<IdxType>::type idx_;

  public:
    using matxop = bool;
    using scalar_type = typename T::scalar_type;
    static_assert(IdxType::Rank() == 1, "Rank of index operator must be 1");

    __MATX_INLINE__ SelectOp(T op, IdxType idx) : op_(op), idx_(idx) {};  
    
    template <typename... Is>
    __MATX_INLINE__ __MATX_DEVICE__ __MATX_HOST__ auto operator()(index_t i) const 
    {
      auto arrs = detail::GetIdxFromAbs(op_, idx_(i));
      return op_(arrs);     
    }

    static __MATX_INLINE__ constexpr __MATX_HOST__ __MATX_DEVICE__ int32_t Rank()
    {
      return detail::get_rank<IdxType>();
    }
    constexpr __MATX_INLINE__ __MATX_HOST__ __MATX_DEVICE__ index_t Size(int dim) const
    {
      return idx_.Size(dim);
    }
  };
  }   
  
  namespace detail {
  template <int CRank, typename T, typename Ind>
  class CloneOp : public BaseOp<CloneOp<CRank, T, Ind>>
  {
    private:
      mutable typename base_type<T>::type op_;
      std::array<index_t, CRank> sizes_;         // size of each dimension after cloning
      std::array<index_t, T::Rank()> dims_;      // gather map for computing operator() indices
    public:
      using matxop = bool;

      using scalar_type = typename T::scalar_type;

      __MATX_INLINE__ CloneOp(T op, std::array<index_t, CRank> shape) : op_(op) {
        // create gather list
        int d = 0;
        for(int i = 0; i < Rank(); i++) {
          if(shape[i]==matxKeepDim) {
            sizes_[i] = op_.Size(d);
            dims_[d++] = i;
          } else {
            sizes_[i] = shape[i];
          }
        }
        MATX_ASSERT(d == T::Rank(), matxInvalidDim);

      };

      template <typename... Is>
        __MATX_INLINE__ __MATX_DEVICE__ __MATX_HOST__ auto operator()(Is... indices) const 
        {

          // convert variadic type to tuple so we can read/update
          std::array<index_t, Rank()> sind{indices...};
          std::array<index_t, T::Rank()> gind;

          // gather indices
          for(int i = 0; i < T::Rank(); i++) {
            auto idx = dims_[i];
            gind[i] = sind[idx];
          }

          return mapply(op_, gind);
        }

      static __MATX_INLINE__ constexpr __MATX_HOST__ __MATX_DEVICE__ int32_t Rank()
      {
        return CRank;
      }
      constexpr __MATX_INLINE__ __MATX_HOST__ __MATX_DEVICE__ index_t Size(int dim) const
      {
        return sizes_[dim];
      }
  
  };
  }

  
/**
 * @brief Operator to clone an operator or tensor acorss dimensions
 *
 * @tparam Rank the rank of the cloned operator
 * @tparam T source operator/tensor type
 * @param t source operator/tensor
 * @param shape the shape of the cloned operator/tensor.  
 * Each element is either the size of the cloned dimension or matxKeepDim to be from the source tensor
 * @return operator to compute the cloned value
 */
  template <int Rank, typename Op>
  auto __MATX_INLINE__ clone(Op t, const index_t (&shape)[Rank])
  {
    std::array<index_t, Rank> lshape;
    for(int i = 0; i < Rank ; i++) {
      lshape[i]=shape[i];
    }
    return detail::CloneOp<Rank, Op, index_t>(t, lshape);
  };   

/**
 * Remaps elements an operator according to an index array/operator.
 */
  namespace detail {
  template <int DIM, typename T, typename IdxType>
  class RemapOp : public BaseOp<RemapOp<DIM, T, IdxType>>
  {
  private:
    //mutable typename base_type<T>::type op_;
    typename base_type<T>::type op_;
    typename base_type<IdxType>::type idx_;

  public:
    using matxop = bool;
    using matxoplvalue = bool;

    using scalar_type = typename T::scalar_type;
    using shape_type = typename T::shape_type; 
    using index_type = typename IdxType::scalar_type;
    static_assert(std::is_integral<index_type>::value, "RemapOp: Type for index operator must be integral");
    static_assert(IdxType::Rank() == 1, "RemapOp: Rank of index operator must be 1");
    static_assert(DIM<T::Rank(), "RemapOp: DIM must be less than Rank of tensor");
    __MATX_INLINE__ RemapOp(T op, IdxType idx) : op_(op), idx_(idx) {};

    template <typename... Is>
    __MATX_INLINE__ __MATX_DEVICE__ __MATX_HOST__ auto operator()(Is... indices) const 
    {
      static_assert(sizeof...(Is)==Rank());
      static_assert((std::is_convertible_v<Is, index_t> && ... ));

      // convert variadic type to tuple so we can read/update
      std::array<index_t, Rank()> ind{indices...};
      // get current index for dim
      auto i = ind[DIM];
      // remap current index for dim
      ind[DIM] = idx_(i);
      //return op_(ind);
      return mapply(op_, ind);
    }
    
    template <typename... Is>
    __MATX_INLINE__ __MATX_DEVICE__ __MATX_HOST__ auto& operator()(Is... indices)
    {
      static_assert(sizeof...(Is)==Rank());
      static_assert((std::is_convertible_v<Is, index_t> && ... ));

      // convert variadic type to tuple so we can read/update
      std::array<index_t, Rank()> ind{indices...};
      // get current index for dim
      auto i = ind[DIM];
      // remap current index for dim
      ind[DIM] = idx_(i);
      //return op_(ind);
      return mapply(op_, ind);
    }

    static __MATX_INLINE__ constexpr __MATX_HOST__ __MATX_DEVICE__ int32_t Rank()
    {
      return T::Rank();
    }
    constexpr __MATX_INLINE__ __MATX_HOST__ __MATX_DEVICE__ index_t Size(int dim) const
    {
      if(dim == DIM)
        return idx_.Size(0);
      else 
        return op_.Size(dim);
    }
  
    template<typename R> __MATX_INLINE__ auto operator=(const R &rhs) { return set(*this, rhs); }
  };
  }

/**
 * @brief Operator to logically remap elements of an operator based on an index array/operator.
 *
 * The rank of the output tensor is equal to the rank of the input tensor.
 * The rank of the index tensor must be 1.  
 *
 * The size of the output tensor is the same as the input tensor except in the applied dimenions.
 * In the applied dimension the size of the output tensor is equal to the size of the index tensor.
 * 
 * This operator can appear as an rvalue or lvalue. 
 *
 * @tparam DIM Dimension to apply the remap 
 * @tparam T Input operator/tensor type
 * @tparam Ind Input index Operator type
 * @param t Input operator
 * @param idx Index operator/tensor
 * @return Value in t from each location in idx
 */
template <int DIM, typename Op, typename Ind>
auto __MATX_INLINE__ remap(Op t, Ind idx)
{
  return detail::RemapOp<DIM, Op, Ind>(t, idx);
};   

/**
 * @brief Operator to logically remap elements of an operator based on an index array/operator.
 *
 * The rank of the output tensor is equal to the rank of the input tensor.
 * The rank of the index tensor must be 1.  
 * The number of DIMS and the number of Inds provided must be the same.
 *
 * The size of the output tensor is the same as the input tensor except in the applied dimenions.
 * In the applied dimension the size of the output tensor is equal to the size of the index tensor.
 * 
 * This operator can appear as an rvalue or lvalue. 
 *
 * @tparam DIM Dimension to apply the remap 
 * @tparam DIMS... list of multiple dimensions to remap along
 * @tparam T Input operator/tensor type
 * @tparam Ind Input index Operator type
 * @tparam Inds... list of multiple index operators to remap along
 * @param t Input operator
 * @param idx Index operator/tensor
 * @return Value in t from each location in idx
 */
template <int DIM, int... DIMS, typename Op, typename Ind, typename... Inds>
auto __MATX_INLINE__ remap(Op t, Ind idx, Inds... inds)
{
  static_assert(sizeof...(DIMS) == sizeof...(Inds), "remap number of DIMs must match number of index arrays");

  // recursively call remap on remaining bits
  auto op = remap<DIMS...>(t, inds...);

  // construct remap op
  return detail::RemapOp<DIM, decltype(op) , Ind>(op, idx);
};   

/**
 * Slices elements from an operator/tensor.
 */
  namespace detail {
  template <int DIM, typename T>
  class SliceOp : public BaseOp<SliceOp<DIM, T>>
  {
    public: 
      using scalar_type = typename T::scalar_type;
      using shape_type = typename T::shape_type; 

    private:
      typename base_type<T>::type op_;
      std::array<shape_type, DIM> sizes_;
      std::array<shape_type, DIM> dims_;
      std::array<shape_type, T::Rank()> starts_;
      std::array<shape_type, T::Rank()> strides_;

    public:
      using matxop = bool;
      using matxoplvalue = bool;

      static_assert(T::Rank()>0, "SliceOp: Rank of operator must be greater than 0.");
      static_assert(DIM<=T::Rank(), "SliceOp: DIM must be less than or equal to operator rank.");

      __MATX_INLINE__ SliceOp(T op, const shape_type (&starts)[T::Rank()], const shape_type (&ends)[T::Rank()], const shape_type (&strides)[T::Rank()]) : op_(op) {
        int d = 0;
        for(int i = 0; i < T::Rank(); i++) {
          shape_type start = starts[i];
          shape_type end = ends[i];

          starts_[i] = start;
          strides_[i] = strides[i];

          // compute dims and sizes
          if(end != matxDropDim) {
            dims_[d] = i;

            if(end == matxEnd) {
              sizes_[d] = op.Size(i) - start;
            } else {
              sizes_[d] = end - start;
            }
          
	    //adjust size by stride
            sizes_[d] = (shape_type)std::ceil(static_cast<double>(sizes_[d])/ static_cast<double>(strides_[d]));
            d++;
          }
        }
        MATX_ASSERT_STR(d==Rank(), matxInvalidDim, "SliceOp: Number of dimensions without matxDropDim must equal new rank.");
      };

      template <typename... Is>
        __MATX_INLINE__ __MATX_DEVICE__ __MATX_HOST__ auto operator()(Is... indices) const 
        {
          static_assert(sizeof...(Is)==Rank());
          static_assert((std::is_convertible_v<Is, index_t> && ... ));

          // convert variadic type to tuple so we can read/update
          std::array<index_t, Rank()> inds{indices...};
          std::array<index_t, T::Rank()> ind{indices...};

#pragma unroll 
          for(int i = 0; i < T::Rank(); i++) {
            ind[i] = starts_[i];
          }

#pragma unroll 
          for(int i = 0; i < Rank(); i++) {
            ind[dims_[i]] += inds[i] * strides_[i]; 
          }

          //return op_(ind);
          return mapply(op_, ind);
        }

      template <typename... Is>
        __MATX_INLINE__ __MATX_DEVICE__ __MATX_HOST__ auto& operator()(Is... indices)
        {
          static_assert(sizeof...(Is)==Rank());
          static_assert((std::is_convertible_v<Is, index_t> && ... ));

          // convert variadic type to tuple so we can read/update
          std::array<index_t, Rank()> inds{indices...};
          std::array<index_t, T::Rank()> ind{indices...};

#pragma unroll 
          for(int i = 0; i < T::Rank(); i++) {
            ind[i] = starts_[i];
          }

#pragma unroll 
          for(int i = 0; i < Rank(); i++) {
            ind[dims_[i]] += inds[i] * strides_[i]; 
          }
          
	  //return op_(ind);
          return mapply(op_, ind);
        }

      static __MATX_INLINE__ constexpr __MATX_HOST__ __MATX_DEVICE__ int32_t Rank()
      {
        return DIM;
      }
      constexpr __MATX_INLINE__ __MATX_HOST__ __MATX_DEVICE__ index_t Size(int dim) const
      {
        return sizes_[dim];
      }

      template<typename R> __MATX_INLINE__ auto operator=(const R &rhs) { return set(*this, rhs); }
  };
  }

  template <typename T>
  __MATX_INLINE__ auto slice( const T op, 
                              const typename T::shape_type (&starts)[T::Rank()],
                              const typename T::shape_type (&ends)[T::Rank()],
                              const typename T::stride_type (&strides)[T::Rank()]) {
    return detail::SliceOp<T::Rank(),T>(op, starts, ends, strides);
  }
  
  template <typename T>
  __MATX_INLINE__ auto slice( const T op, 
                              const typename T::shape_type (&starts)[T::Rank()],
                              const typename T::shape_type (&ends)[T::Rank()]) {
    typename T::shape_type strides[T::Rank()];
    for(int i = 0; i < T::Rank(); i++)
      strides[i] = 1;
    return detail::SliceOp<T::Rank(),T>(op, starts, ends, strides);
  }

/**
 * @brief Operator to logically slice a tensor or operator.
 *
 * The rank of the the operator must be greater than 0.
 
 * This operator can appear as an rvalue or lvalue. 
 *
 * @tparam N The Rank of the output operator
 * @tparam T Input operator/tensor type
 * @param Op Input operator
 * @param starts the first element (inclusive) of each dimension of the input operator.
 * @param ends the last element (exclusive) of each dimension of the input operator.  matxDrop Dim removes that dimension.  matxEnd deontes all remaining elements in that dimension.
 * @param strides Optional:  the stride between consecutive elements
 * @return sliced operator
 */
  template <int N, typename T>
  __MATX_INLINE__ auto slice( const T op, 
                              const typename T::shape_type (&starts)[T::Rank()],
                              const typename T::shape_type (&ends)[T::Rank()],
                              const typename T::stride_type (&strides)[T::Rank()]) {
    return detail::SliceOp<N,T>(op, starts, ends, strides);
  }
  
  template <int N, typename T>
  __MATX_INLINE__ auto slice( const T op, 
                              const typename T::shape_type (&starts)[T::Rank()],
                              const typename T::shape_type (&ends)[T::Rank()]) {
    typename T::shape_type strides[T::Rank()];
    for(int i = 0; i < T::Rank(); i++)
      strides[i] = 1;
    return detail::SliceOp<N,T>(op, starts, ends, strides);
  }
  

/**
 * @brief Helper function to select values from a predicate operator
 * 
 * select() is used to index from a source operator using indices stored
 * in another operator. This is commonly used with the find_idx executor 
 * which returns the indices of values meeting a selection criteria.
 * 
 * @tparam T Input type
 * @tparam IdxType Operator with indices
 * @param t Input operator
 * @param idx Index tensor
 * @return Value in t from each location in idx
 */
template <typename T, typename IdxType>
auto __MATX_INLINE__ select(T t, IdxType idx)
{
  return detail::SelectOp<T, IdxType>(t, idx);
};   



/**
 * Casts the element of the tensor to a specified type
 *
 * Useful when performing type conversions inside of larger expressions
 *
 */
  namespace detail {
  template <typename T, typename NewType>
  class CastOp : public BaseOp<CastOp<T, NewType>>
  {
  private:
    typename base_type<T>::type op_;

  public:
    using matxop = bool;
    using scalar_type = NewType;

    __MATX_INLINE__ CastOp(T op) : op_(op){};  
    
    template <typename... Is>
    __MATX_INLINE__ __MATX_DEVICE__ __MATX_HOST__ auto operator()(Is... indices) const 
    {
      return static_cast<NewType>(op_(indices...));     
    }

    static __MATX_INLINE__ constexpr __MATX_HOST__ __MATX_DEVICE__ int32_t Rank()
    {
      return detail::get_rank<T>();
    }
    constexpr __MATX_INLINE__ __MATX_HOST__ __MATX_DEVICE__ index_t Size(int dim) const
    {
      return op_.Size(dim);
    }
  };
  }   


/**
 * @brief Helper function to cast an input operator to a different type
 * 
 * @tparam T Input type
 * @tparam NewType Casted type
 * @param t Input operator
 * @return Operator output casted to NewType 
 */
template <typename NewType, typename T>
auto __MATX_INLINE__ as_type(T t)
{
  return detail::CastOp<T, NewType>(t);
};   


/**
 * @brief Helper function to cast an input operator to an int
 * 
 * @tparam T Input type
 * @param t Input operator
 * @return Operator output casted to int 
 */
template <typename T>
auto __MATX_INLINE__ as_int(T t)
{
  return detail::CastOp<T, int>(t);
};   

/**
 * @brief Helper function to cast an input operator to an float
 * 
 * @tparam T Input type
 * @param t Input operator
 * @return Operator output casted to float 
 */
template <typename T>
auto __MATX_INLINE__ as_float(T t)
{
  return detail::CastOp<T, float>(t);
};   

/**
 * @brief Helper function to cast an input operator to an double
 * 
 * @tparam T Input type
 * @param t Input operator
 * @return Operator output casted to double 
 */
template <typename T>
auto __MATX_INLINE__ as_double(T t)
{
  return detail::CastOp<T, double>(t);
};   

/**
 * @brief Helper function to cast an input operator to an uint32_t
 * 
 * @tparam T Input type
 * @param t Input operator
 * @return Operator output casted to uint32_t 
 */
template <typename T>
auto __MATX_INLINE__ as_uint32(T t)
{
  return detail::CastOp<T, uint32_t>(t);
};   

/**
 * @brief Helper function to cast an input operator to an int32_t
 * 
 * @tparam T Input type
 * @param t Input operator
 * @return Operator output casted to int32_t 
 */
template <typename T>
auto __MATX_INLINE__ as_int32(T t)
{
  return detail::CastOp<T, int32_t>(t);
}; 

/**
 * @brief Helper function to cast an input operator to an int16_t
 * 
 * @tparam T Input type
 * @param t Input operator
 * @return Operator output casted to int16_t 
 */
template <typename T>
auto __MATX_INLINE__ as_int16(T t)
{
  return detail::CastOp<T, int16_t>(t);
}; 

/**
 * @brief Helper function to cast an input operator to an uint16_t
 * 
 * @tparam T Input type
 * @param t Input operator
 * @return Operator output casted to uint16_t 
 */
template <typename T>
auto __MATX_INLINE__ as_uint16(T t)
{
  return detail::CastOp<T, uint16_t>(t);
}; 

/**
 * @brief Helper function to cast an input operator to an int8_t
 * 
 * @tparam T Input type
 * @param t Input operator
 * @return Operator output casted to int8_t 
 */
template <typename T>
auto __MATX_INLINE__ as_int8(T t)
{
  return detail::CastOp<T, int8_t>(t);
}; 

/**
 * @brief Helper function to cast an input operator to an uint8_t
 * 
 * @tparam T Input type
 * @param t Input operator
 * @return Operator output casted to uint8_t 
 */
template <typename T>
auto __MATX_INLINE__ as_uint8(T t)
{
  return detail::CastOp<T, uint8_t>(t);
}; 



/**
 * Reverse the indexing of a View or operator on a single dimension
 *
 * Allows a view or operator to be indexed in reverse order. After applying the
 * operator, index 0 is the last element in the selected dimension, index 1 is
 * second to last, etc.
 *
 */
  namespace detail {
  template <int DIM, typename T1>
  class ReverseOp : public BaseOp<ReverseOp<DIM, T1>>
  {
  private:
    typename base_type<T1>::type op_;

  public:
    using matxop = bool;
    using matxoplvalue = bool;
    using scalar_type = typename T1::scalar_type;

    __MATX_INLINE__ ReverseOp(T1 op) : op_(op){};

    
    template <typename... Is>
    __MATX_INLINE__ __MATX_DEVICE__ __MATX_HOST__ auto operator()(Is... indices) const 
    {
      if constexpr (Rank() == 0) {
        return op_();
      } 
      else {
        auto tup = cuda::std::make_tuple(indices...);
        cuda::std::get<DIM>(tup) = Size(DIM) - cuda::std::get<DIM>(tup) - 1;
        return mapply(op_, tup);
      }

      if constexpr (Rank() != 0) {
        auto tup = cuda::std::make_tuple(indices...);
        cuda::std::get<DIM>(tup) = Size(DIM) - cuda::std::get<DIM>(tup) - 1;
        return mapply(op_, tup);
      } 
      else {
        return op_();
      }      
    }

    static __MATX_INLINE__ constexpr __MATX_HOST__ __MATX_DEVICE__ int32_t Rank()
    {
      return detail::get_rank<T1>();
    }
    constexpr __MATX_INLINE__ __MATX_HOST__ __MATX_DEVICE__ index_t Size(int dim) const
    {
      return op_.Size(dim);
    }
    
    template<typename R> __MATX_INLINE__ auto operator=(const R &rhs) { return set(*this, rhs); }
  };
  }

/**
 * @brief Operator to logically reverse elements of an operator. Base case for variadic template.
 *
 * @tparam DIM Dimension to apply the reverse
 * @tparam Op Input operator/tensor type
 * @param t Input operator
 */
  template <int DIM, typename Op>
  auto __MATX_INLINE__ reverse(Op t)
  {
    return detail::ReverseOp<DIM, Op>(t);
  };

/**
 * @brief Operator to logically reverse elements of an operator.
 *
 * This operator can appear as an rvalue or lvalue.
 *
 * @tparam DIM Dimension to apply the reverse
 * @tparam DIMS... list of multiple dimensions to reverse along
 * @tparam Op Input operator/tensor type
 * @param t Input operator
 */
template <int DIM1, int DIM2, int... DIMS, typename Op>
auto __MATX_INLINE__ reverse(Op t)
{
  // recursively call remap on remaining bits
  auto op = reverse<DIM2, DIMS...>(t);

  // construct remap op
  return detail::ReverseOp<DIM1, decltype(op)>(op);
};

  /**
 * Flip the vertical axis of a tensor.
 */
  template <typename T1>
  auto __MATX_INLINE__ flipud(T1 t)
  {
    if constexpr (T1::Rank() == 1)
    {
      return detail::ReverseOp<T1::Rank() - 1 , T1>(t);
    }

    return detail::ReverseOp<T1::Rank() - 2, T1>(t);
  };

  /**
 * Flip the horizontal axis of a tensor.
 */
  template <typename T1>
  auto __MATX_INLINE__ fliplr(T1 t)
  {
    if constexpr (T1::Rank() == 1)
    {
      return detail::ReverseOp<T1::Rank() - 1, T1>(t);
    }

    return detail::ReverseOp<T1::Rank() - 1, T1>(t);
  };

  /**
 * Performs a Hermitian transpose operator on a tensor
 *
 * This operation allows a user to perform a Hermitian operator using a
 * single operator instead of Permute followed by a conj() operator.
 */
  namespace detail {
  template <typename T1, int DIM>
  class HermitianTransOp : public BaseOp<HermitianTransOp<T1, DIM>>
  {
  private:
    typename base_type<T1>::type op_;
    
  public:
    using matxop = bool;
    using scalar_type = typename T1::scalar_type;

    __MATX_INLINE__ HermitianTransOp(T1 op) : op_(op) {}

    template <typename... Is>
    __MATX_INLINE__ __MATX_DEVICE__ __MATX_HOST__ auto operator()(Is... indices) const 
    {
      auto tup = cuda::std::make_tuple(indices...);
      return conj(mapply_reverse(op_, tup));
    }

    static __MATX_INLINE__ constexpr __MATX_HOST__ __MATX_DEVICE__ int32_t Rank()
    {
      return detail::get_rank<T1>();
    }
    constexpr __MATX_INLINE__ __MATX_HOST__ __MATX_DEVICE__ index_t Size(int dim) const
    {
      return op_.Size(Rank() - dim - 1);
    }
  };
  }

  /**
 * Helper function for creating a hermitian transpose from an operator/View
 */
  template <typename T1>
  auto __MATX_INLINE__ hermitianT(T1 t)
  {
    return detail::HermitianTransOp<T1, T1::Rank()>(t);
  }

  /**
 * Returns elements on the diagonal
 *
 * Returns elements on the diagonal of a 2D tensor. Any dimensions above 2 will
 * be considered batch dimension and the size of those match the size of the
 * input operator. The last dimension is always sized to be the minimum of the
 * last two dimension of the input operator
 */
  namespace detail {
  template <typename T1, int RANK>
  class DiagOp : public BaseOp<DiagOp<T1, RANK>>
  {
  private:
    typename base_type<T1>::type op_;

  public:
    using matxop = bool;
    using scalar_type = typename T1::scalar_type;

    __MATX_INLINE__ DiagOp(T1 op) : op_(op) {}

    template <typename... Is>
    __MATX_INLINE__ __MATX_DEVICE__ __MATX_HOST__ auto operator()(Is... indices) const 
    {
      static_assert(sizeof...(Is) == RANK - 1, "Diagonal operator must have one fewer index than rank of operator");
      static_assert(RANK > 1, "Cannot make get diagonals from 0D tensor");

      auto tup = cuda::std::make_tuple(indices..., 0);
      cuda::std::get<RANK - 1>(tup) = pp_get<RANK-2>(indices...);
      return mapply(op_, tup);
    }

    static __MATX_INLINE__ constexpr __MATX_HOST__ __MATX_DEVICE__ int32_t Rank()
    {
      return RANK - 1;
    }

    constexpr __MATX_INLINE__ __MATX_HOST__ __MATX_DEVICE__ index_t Size([[maybe_unused]] int dim) const
    {
      if (dim < RANK - 2) {
        return op_.Size(dim);
      }
      else {
        return std::min(op_.Size(RANK - 1), op_.Size(RANK-2));
      }
    }
  };
  }

  /**
 * Get the elements on the diagonal
 *
 * @param t
 *   Input operator
 */
  template <typename T1>
  auto __MATX_INLINE__ diag(T1 t) { return detail::DiagOp<T1, T1::Rank()>(t); }
  
  namespace detail {
  template <int DIM, typename T1>
  class LCollapseOp : public BaseOp<LCollapseOp<DIM, T1>>
  {
  private:
    typename base_type<T1>::type op_;
    index_t size_;  // size of collapsed dim

  public:
    using matxop = bool;
    using scalar_type = typename T1::scalar_type;

    __MATX_INLINE__ LCollapseOp(const T1 &op) : op_(op)
    {
      static_assert(DIM < T1::Rank(),  "Collapse DIM must be less than Rank() of operator");
      static_assert(DIM > 0, "Collapse DIM must have a magnitude greater than 0");
      static_assert(T1::Rank() > 1, "Collapse must be called on operators with rank > 1");

      // comptue size of collapsed dimension
      size_ = 1;
      
      // Collapse right-most dims
      #pragma unroll
      for(int i = 0 ; i <= DIM; i++) {
        size_ *= op_.Size(i);
      }
    }
    
    template <typename... Is>
    __MATX_INLINE__ __MATX_DEVICE__ __MATX_HOST__ auto operator()(Is... indices) const 
    {
      // indices coming in
      std::array<index_t, Rank()> in{indices...};  // index coming in
      std::array<index_t, T1::Rank()> out;         // index going out

      #pragma unroll
      for(int i = 1; i < Rank(); i++) {
        // copy all but first input index into out array
        out[DIM+i] = in[i];
      }
       
      // expand first input index into DIM indices
      auto ind = in[0];
      #pragma unroll
      for(int i = 0; i <= DIM; i++) {
        index_t d = DIM - i;
        out[d] = ind % op_.Size(d);
        ind /= op_.Size(d);
      }
      
      return mapply(op_, out);
    }    

    static __MATX_INLINE__ constexpr __MATX_HOST__ __MATX_DEVICE__ int32_t Rank()
    {
      return T1::Rank() - DIM;
    }

    constexpr __MATX_INLINE__ __MATX_HOST__ __MATX_DEVICE__ index_t Size(int dim) const
    {
      if(dim == 0)  // if asking for the first dim, return collapsed size
        return size_;
      else // otherwise return the un-collapsed size from operator
        return op_.Size(DIM+dim);
    }
  };
  }
/**
 * lcollapse operator
 *
 * The lcollapse operator takes a tensor and collapses the left most dimensions into a single dimension.
 *
 * @tparam DIM
 *   The number of dimensions to collapse
 * @tparam T1
 *   Operator type
 *
 * @param a
 *   The operator being collapsed
 *
 * @returns
 *   Operator with collapsed input
 */
  template <int DIM, typename T1>
  auto __MATX_INLINE__ lcollapse(const T1 &a)
  {
    return detail::LCollapseOp<DIM, T1>(a);
  }
  
  namespace detail {
  template <int DIM, typename T1>
  class RCollapseOp : public BaseOp<RCollapseOp<DIM, T1>>
  {
  private:
    typename base_type<T1>::type op_;
    index_t size_;  // size of collapsed dim

  public:
    using matxop = bool;
    using scalar_type = typename T1::scalar_type;
    using matxlvalue = bool;

    __MATX_INLINE__ RCollapseOp(const T1 &op) : op_(op)
    {
      static_assert(DIM < T1::Rank(),  "Collapse DIM must be less than Rank() of operator");
      static_assert(DIM > 0, "Collapse DIM must have a magnitude greater than 0");
      static_assert(T1::Rank() > 1, "Collapse must be called on operators with rank > 1");

      // comptue size of collapsed dimension
      size_ = 1;
      
      // Collapse right-most dims
      #pragma unroll
      for(int i = 0 ; i <= DIM; i++) {
        size_ *= op_.Size(T1::Rank() - 1 - i);
      }
    }
    
    template <typename... Is>
    __MATX_INLINE__ __MATX_DEVICE__ __MATX_HOST__ auto operator()(Is... indices) const 
    {
      // indices coming in
      std::array<index_t, Rank()> in{indices...};  // index coming in
      std::array<index_t, T1::Rank()> out;         // index going out

      #pragma unroll
      for(int i = 0 ; i < Rank() - 1; i++) {
        // copy all but last index into out array
        out[i] = in[i];
      }
        
      // expand last index into DIM indices
      auto ind = in[Rank() - 1];
      #pragma unroll
      for(int i = 0; i <= DIM; i++) {
        index_t d = T1::Rank() - 1 - i;
        out[d] = ind % op_.Size(d);
        ind /= op_.Size(d);
      }
      
      return mapply(op_, out);
    }    

    static __MATX_INLINE__ constexpr __MATX_HOST__ __MATX_DEVICE__ int32_t Rank()
    {
      return T1::Rank() - DIM;
    }

    constexpr __MATX_INLINE__ __MATX_HOST__ __MATX_DEVICE__ index_t Size(int dim) const
    {
      if(dim == Rank()-1)  // if asking for the last dim, return collapsed size
        return size_;
      else // otherwise return the un-collapsed size from operator
        return op_.Size(dim);
    }
  };
  }
/**
 * rcollapse operator
 *
 * The rcollapse operator takes a tensor and collapses the right most dimensions into a single dimension.
 *
 * @tparam DIM
 *   The number of dimensions to collapse
 * @tparam T1
 *   Operator type
 *
 * @param a
 *   The parameter being collapsed
 *
 * @returns
 *   Operator with collapsed input
 */
  template <int DIM, typename T1>
  auto __MATX_INLINE__ rcollapse(const T1 &a)
  {
    return detail::RCollapseOp<DIM, T1>(a);
  }

  namespace detail {
  template <typename T1>
  class FlattenOp : public BaseOp<FlattenOp<T1>>
  {
  private:
    typename base_type<T1>::type op1_;

  public:
    using matxop = bool;
    using scalar_type = typename T1::scalar_type;

    __MATX_INLINE__ FlattenOp(const T1 &op1) : op1_(op1)
    {
      static_assert(T1::Rank() > 1, "flatten has no effect on tensors of rank 0 and 1");
    }

    template <typename Is>
    __MATX_INLINE__ __MATX_DEVICE__ __MATX_HOST__ auto operator()(Is id0) const 
    {
      return *RandomOperatorIterator{op1_, id0};
    }    

    static __MATX_INLINE__ constexpr __MATX_HOST__ __MATX_DEVICE__ int32_t Rank()
    {
      return 1;
    }

    constexpr __MATX_INLINE__ __MATX_HOST__ __MATX_DEVICE__ index_t Size(int dim) const
    {
      index_t size = 1;
      if (dim == 0) {
        for (int r = 0; r < op1_.Rank(); r++) {
          size *= op1_.Size(r);
        }
      }

      return size;
    }
  };
  }

/**
 * Flatten operator
 *
 * The flatten operator takes an operator of rank 2 or higher and flattens every dimension
 * into a single 1D tensor. 
 *
 * @tparam T1
 *   Operator type
 *
 * @returns
 *   Operator of flattened input
 */
  template <typename T1>
  auto __MATX_INLINE__ flatten(const T1 &a)
  {
    return detail::FlattenOp<T1>(a);
  };


  /**
 * Kronecker tensor product
 *
 * Performs a Kronecker tensor product on two matrices. For input tensors A
 * (MxN) and B (PxQ), A is repeated and multiplied by each element in B to
 * create a new matrix of size M*P x N*Q.
 */
  namespace detail {
  template <typename T1, typename T2, int DIM>
  class KronOp : public BaseOp<KronOp<T1, T2, DIM>>
  {
  private:
    typename base_type<T1>::type op1_;
    typename base_type<T2>::type op2_;

  public:
    using matxop = bool;
    using scalar_type = typename T1::scalar_type;

    __MATX_INLINE__ KronOp(T1 op1, T2 op2) : op1_(op1), op2_(op2)
    {
      static_assert(Rank() >= 2, "Kronecker product must be used on tensors with rank 2 or higher");
    }

    template <typename... Is>
    __MATX_INLINE__ __MATX_DEVICE__ __MATX_HOST__ auto operator()(Is... indices) const 
    {
      auto tup1 = cuda::std::make_tuple(indices...);
      auto tup2 = cuda::std::make_tuple(indices...);
      cuda::std::get<Rank() - 2>(tup2) = pp_get<Rank() - 2>(indices...) % op2_.Size(Rank() - 2);
      cuda::std::get<Rank() - 1>(tup2) = pp_get<Rank() - 1>(indices...) % op2_.Size(Rank() - 1);

      cuda::std::get<Rank() - 2>(tup1) = pp_get<Rank() - 2>(indices...) / op2_.Size(Rank() - 2);
      cuda::std::get<Rank() - 1>(tup1) = pp_get<Rank() - 1>(indices...) / op2_.Size(Rank() - 1);      

      return mapply(op2_, tup2) * mapply(op1_, tup1);
    }    

    static __MATX_INLINE__ constexpr __MATX_HOST__ __MATX_DEVICE__ int32_t Rank()
    {
      return detail::get_rank<T1>();
    }
    constexpr __MATX_INLINE__ __MATX_HOST__ __MATX_DEVICE__ index_t Size(int dim) const
    {
      return op1_.Size(dim) * op2_.Size(dim);
    }
  };
  }

  /**
 * Kronecker tensor product
 *
 * The Kronecker tensor product is formed by the matrix b by ever element in the
 * matrix a. The resulting matrix has the number of rows and columns equal to
 * the product of the rows and columns of matrices a and b, respectively.
 *
 * @tparam T1
 *   Type of first input
 * @tparam T2
 *   Type of second input
 * @param a
 *   Operator or view for first input
 * @param b
 *   Operator or view for second input
 *
 * @returns
 *   New operator of the kronecker product
 */
  template <typename T1, typename T2>
  auto __MATX_INLINE__ kron(T1 a, T2 b)
  {
    return detail::KronOp<T1, T2, T1::Rank()>(a, b);
  };

  /**
 * Repeats a matrix the specified amount of times
 *
 * RepMatOp performs a "repmat" operation on a matrix where each dimension
 * specified in "reps" is repeated. Constructors for both scalars and arrays are
 * provided. The scalar version will repeat the matrix by the scalar amount in
 * every dimension, whereas the array version scales independently by each
 * dimension.
 */
  namespace detail {
  template <typename T1, int DIM>
  class RepMatOp : public BaseOp<RepMatOp<T1, DIM>>
  {
  private:
    typename base_type<T1>::type op_;
    index_t reps_[MAX_TENSOR_DIM];

  public:
    using matxop = bool;
    using scalar_type = typename T1::scalar_type;

    __MATX_INLINE__ RepMatOp(T1 op, index_t reps) : op_(op)
    {
      for (int dim = 0; dim < DIM; dim++)
      {
        reps_[dim] = reps;
      }
    }

    __MATX_INLINE__ RepMatOp(T1 op, const std::array<index_t, DIM> reps) : op_(op)
    {
      for (int dim = 0; dim < DIM; dim++)
      {
        reps_[dim] = reps[dim];
      }
    }

    __MATX_INLINE__ RepMatOp(T1 op, const index_t *reps) : op_(op)
    {
      for (int dim = 0; dim < DIM; dim++)
      {
        reps_[dim] = reps[dim];
      }
    }


    template <int I = 0, typename ...Is>
    __MATX_INLINE__ __MATX_HOST__ __MATX_DEVICE__ void UpdateIndex(cuda::std::tuple<Is...> &tup) const {
      if constexpr (I != sizeof...(Is)) {
        cuda::std::get<I>(tup) %= op_.Size(I);
        UpdateIndex<I+1, Is...>(tup);
      }
    }

    template <typename... Is>
    __MATX_INLINE__ __MATX_DEVICE__ __MATX_HOST__ auto operator()(Is... indices) const 
    {
      if constexpr (Rank() == 0) {
        return op_();
      }
      else {
        auto tup = cuda::std::make_tuple(indices...);
        UpdateIndex(tup);
        return mapply(op_, tup);
      }

      if constexpr (Rank() != 0) {
        auto tup = cuda::std::make_tuple(indices...);
        UpdateIndex(tup);
        return mapply(op_, tup);
      }
      else {
        return op_();
      }      
    }

    static __MATX_INLINE__ constexpr __MATX_HOST__ __MATX_DEVICE__ int32_t Rank()
    {
      return detail::get_rank<T1>();
    }

    constexpr __MATX_INLINE__ __MATX_HOST__ __MATX_DEVICE__ index_t Size(int dim) const
    {
      return op_.Size(dim) * reps_[dim];
    }
  };
  }

  /**
 * Repeat a matrix an equal number of times in each dimension
 *
 * @tparam T1
 *   Type of operator or view
 * @param t
 *   Operator or view to repeat
 * @param reps
 *   Amount to repeat
 *
 * @returns
 *   New operator with repeated data
 */
  template <typename T1>
  auto __MATX_INLINE__ repmat(T1 t, index_t reps)
  {
    return detail::RepMatOp<T1, T1::Rank()>(t, reps);
  };

  /**
 * Repeat a matrix a specific number of times in each direction
 *
 * @tparam T1
 *   Type of operator or view
 * @param t
 *   Operator or view to repeat
 * @param reps
 *   Array of times to repeat in each dimension
 *
 * @returns
 *   New operator with repeated data
 */
  template <typename T1>
  auto __MATX_INLINE__ repmat(T1 t, const index_t (&reps)[])
  {
    return detail::RepMatOp<T1, T1::Rank()>(t, reps);
  };

  /**
 * Repeat a matrix a specific number of times in each direction
 *
 * @tparam T1
 *   Type of operator or view
 * @param t
 *   Operator or view to repeat
 * @param reps
 *   Array of times to repeat in each dimension
 *
 * @returns
 *   New operator with repeated data
 */
  template <typename T1>
  auto __MATX_INLINE__ repmat(T1 t, const index_t *reps)
  {
    return detail::RepMatOp<T1, T1::Rank()>(t, reps);
  };

  /**
 * Self operator
 *
 * Returns the values of itself. This is useful when converting a type like a
 * tensor view into an operator
 */
  namespace detail {
  template <typename T1, int DIM>
  class SelfOp : public BaseOp<SelfOp<T1, DIM>>
  {
  private:
    typename base_type<T1>::type op_;

  public:
    using matxop = bool;
    using scalar_type = typename T1::scalar_type;

    __MATX_INLINE__ SelfOp(T1 op) : op_(op) {}

    template <typename... Is>
    __MATX_INLINE__ __MATX_DEVICE__ __MATX_HOST__ auto operator()(Is... indices) const 
    {
      return op_(indices...);
    }

    static __MATX_INLINE__ constexpr __MATX_HOST__ __MATX_DEVICE__ int32_t Rank()
    {
      return detail::get_rank<T1>();
    }

    constexpr __MATX_INLINE__ __MATX_HOST__ __MATX_DEVICE__ index_t Size(int dim) const
    {
      return op_.Size(dim);
    }
  };
  }

 /**
 * Returns the values of itself. This is useful when converting a type like a
 * tensor view into an operator
 *
 * @tparam T1
 *   Type of operator or view
 * @param t
 *   Operator or view to access
 *
 * @returns
 *   Operator of input
 */
  template <typename T1>
  auto self(T1 t) { return detail::SelfOp<T1, T1::Rank()>(t); };

  /**
 * Shifts the indexing of an operator to move the array forward or backward by the
 * shift amount. 
 *
 * ShiftOp allows adjusting the relative view of a tensor to start at a
 * new offset. This may be useful to cut off part of a tensor that is
 * meaningless, while maintaining a 0-based offset from the new location. A
 * modulo is applied to the new index to allow wrapping around to the beginning.
 * Negative shifts are allowed, and have the effect of moving back from the end
 * of the tensor.
 */
  namespace detail {
  template <int DIM, typename T1, typename T2>
  class ShiftOp : public BaseOp<ShiftOp<DIM, T1, T2>>
  {
  private:
    typename base_type<T1>::type op_;
    T2 shift_;

  public:
    using matxop = bool;
    using matxoplvalue = bool;
    using scalar_type = typename T1::scalar_type;

    __MATX_INLINE__ ShiftOp(T1 op, T2 shift) : op_(op), shift_(shift)
    {
      static_assert(DIM < Rank(), "Dimension to shift must be less than rank of tensor");
    }

    template <typename... Is>
    __MATX_INLINE__ __MATX_DEVICE__ __MATX_HOST__ auto operator()(Is... indices) const 
    {
      auto tup = cuda::std::make_tuple(indices...);
      auto shift = -get_value(shift_, indices...);


      shift = (shift + cuda::std::get<DIM>(tup)) % Size(DIM);

      if(shift<0) shift+= Size(DIM);

      cuda::std::get<DIM>(tup) = shift;
      
      return mapply(op_, tup);
    }    

    static __MATX_INLINE__ constexpr __MATX_HOST__ __MATX_DEVICE__ int32_t Rank()
    {
      return detail::get_rank<T1>();
    }

    constexpr __MATX_INLINE__ __MATX_HOST__ __MATX_DEVICE__ auto Size(int dim) const noexcept
    {
      return op_.Size(dim);
    }
    
    template<typename R> __MATX_INLINE__ auto operator=(const R &rhs) { return set(*this, rhs); }
  };
  }
  /**
 * Operator to shift dimension by a given amount
 *
 * @tparam DIM
 *   The dimension to be shifted
 *
 * @tparam OpT
 *   Type of operator or view
 *
 * @tparam ShiftOpT
 *   Type of the operator for the shift
 *
 * @param op
 *   Operator or view to shift
 *
 * @param s
 *   Operator which returns the shift
 *
 * @returns
 *   New operator with shifted indices
 */
  template <int DIM, typename OpT, typename ShiftOpT>
  auto __MATX_INLINE__ shift(OpT op, ShiftOpT s)
  {
    return detail::ShiftOp<DIM, OpT, ShiftOpT>(op, s);
  };

  
 /**
 * Operator to shift dimension by a given amount.
 * This version allows multiple dimensions.
 *
 * @tparam DIM
 *   The dimension to be shifted
 *
 * @tparam DIMS...
 *   The dimensions targeted for shifts
 *
 * @tparam OpT
 *   Type of operator or view
 *
 * @tparam ShiftsT
 *   Type of the shift operators
 *
 * @param op
 *   Operator or view to shift
 *
 * @param s
 *   Amount to shift forward
 *
 * @returns
 *   New operator with shifted indices
 */
  template <int DIM, int... DIMS,  typename OpT, typename ShiftT,  typename... ShiftsT>
  auto __MATX_INLINE__ shift(OpT op, ShiftT s, ShiftsT... shifts)
  {
    static_assert(sizeof...(DIMS) == sizeof...(shifts), "shift: number of DIMs must match number of shifts");

    // recursively call shift  on remaining bits
    auto rop = shift<DIMS...>(op, shifts...);

    // construct shift op
    return detail::ShiftOp<DIM, decltype(rop), decltype(s)>(rop, s);
  };

  namespace detail {
  template <typename T1>
  class FFTShift1DOp : public BaseOp<FFTShift1DOp<T1>>
  {
  private:
    typename base_type<T1>::type op_;

  public:
    using matxop = bool;
    using scalar_type = typename T1::scalar_type; 

    __MATX_INLINE__ FFTShift1DOp(T1 op) : op_(op){
      static_assert(Rank() >= 1, "1D FFT shift must have a rank 1 operator or higher");
    };

    template <typename... Is>
    __MATX_INLINE__ __MATX_DEVICE__ __MATX_HOST__ auto operator()(Is... indices) const 
    {
      auto tup = cuda::std::make_tuple(indices...);
      cuda::std::get<Rank()-1>(tup) = (cuda::std::get<Rank()-1>(tup) + (Size(Rank()-1) + 1) / 2) % Size(Rank()-1);
      return mapply(op_, tup);
    }   

    static __MATX_INLINE__ constexpr __MATX_HOST__ __MATX_DEVICE__ int32_t Rank()
    {
      return detail::get_rank<T1>();
    }

    constexpr __MATX_INLINE__ __MATX_HOST__ __MATX_DEVICE__ auto Size(int dim) const noexcept
    {
      return op_.Size(dim);
    }
  };
  }

  /**
 * Perform an FFTShift operation on the last dimension of a tensor
 *
 * Shifts the new indexing of the tensor's last dimension to begin at
 * Size()/2. MatX FFTs leave the sample order starting with DC, positive
 * frequencies, then negative frequencies last. FFTShift gives a shifted
 * view of a signal where the new order is negative frequencies, DC, then
 * positive frequencies.
 *
 * @tparam T1
 *   Type of View/Op
 * @param t
 *   View/Op to shift
 *
 */
  template <typename T1>
  auto fftshift1D(T1 t) { return detail::FFTShift1DOp<T1>(t); }


  namespace detail {
  template <typename T1>
  class FFTShift2DOp : public BaseOp<FFTShift2DOp<T1>>
  {
  private:
    typename base_type<T1>::type op_;

  public:
    using matxop = bool;
    using scalar_type = typename T1::scalar_type;

    __MATX_INLINE__ FFTShift2DOp(T1 op) : op_(op){
      static_assert(Rank() >= 2, "2D FFT shift must have a rank 2 operator or higher");
    };

    template <typename... Is>
    __MATX_INLINE__ __MATX_DEVICE__ __MATX_HOST__ auto operator()(Is... indices) const 
    {
      auto tup = cuda::std::make_tuple(indices...);
      cuda::std::get<Rank()-2>(tup) = (cuda::std::get<Rank()-2>(tup) + (Size(Rank()-2) + 1) / 2) % Size(Rank()-2);
      cuda::std::get<Rank()-1>(tup) = (cuda::std::get<Rank()-1>(tup) + (Size(Rank()-1) + 1) / 2) % Size(Rank()-1);
      return mapply(op_, tup);
    }   

    static __MATX_INLINE__ constexpr __MATX_HOST__ __MATX_DEVICE__ int32_t Rank()
    {
      return detail::get_rank<T1>();
    }

    constexpr __MATX_INLINE__ __MATX_HOST__ __MATX_DEVICE__ auto Size(int dim) const noexcept
    {
      return op_.Size(dim);
    }
  };
  }

  /**
 * Perform an IFFTShift operation on a 2D tensor swapping the first quadrant
 * with the third, and the second with the fourth.
 *
 * Shifts the new indexing of the tensor's last dimension to begin at
 * Size()/2. MatX FFTs leave the sample order starting with DC, positive
 * frequencies, then negative frequencies last. IFFTShift gives a shifted
 * view of a signal where the new order is negative frequencies, DC, then
 * positive frequencies.
 *
 * @tparam T1
 *   Type of View/Op
 * @param t
 *   View/Op to shift
 *
 */
  template <typename T1>
  auto fftshift2D(T1 t) { return detail::FFTShift2DOp<T1>(t); }

  namespace detail {
  template <typename T1>
  class IFFTShift1DOp : public BaseOp<IFFTShift1DOp<T1>>
  {
  private:
    typename base_type<T1>::type op_;

  public:
    using matxop = bool;
    using scalar_type = typename T1::scalar_type;

    __MATX_INLINE__ IFFTShift1DOp(T1 op) : op_(op) {
      static_assert(Rank() >= 1, "1D IFFT shift must have a rank 1 operator or higher");
    };

    template <typename... Is>
    __MATX_INLINE__ __MATX_DEVICE__ __MATX_HOST__ auto operator()(Is... indices) const 
    {
      auto tup = cuda::std::make_tuple(indices...);
      cuda::std::get<Rank()-1>(tup) = (cuda::std::get<Rank()-1>(tup) + (Size(Rank()-1)) / 2) % Size(Rank()-1);
      return mapply(op_, tup);
    } 

    static __MATX_INLINE__ constexpr __MATX_HOST__ __MATX_DEVICE__ int32_t Rank()
    {
      return detail::get_rank<T1>();
    }

    constexpr __MATX_INLINE__ __MATX_HOST__ __MATX_DEVICE__ auto Size(int dim) const noexcept
    {
      return op_.Size(dim);
    }
  };
  }

  /**
 * Perform an IFFTShift operation on the last dimension of a tensor
 *
 * Shifts the new indexing of the tensor's last dimension to begin at
 * Size()/2. MatX FFTs leave the sample order starting with DC, positive
 * frequencies, then negative frequencies last. IFFTShift gives a shifted
 * view of a signal where the new order is negative frequencies, DC, then
 * positive frequencies. Note that ifftshift is the same as fftshift if the
 * length of the signal is even.
 *
 * @tparam T1
 *   Type of View/Op
 * @param t
 *   View/Op to shift
 *
 */
  template <typename T1>
  auto ifftshift1D(T1 t) { return detail::IFFTShift1DOp<T1>(t); }

  namespace detail {
  template <typename T1>
  class IFFTShift2DOp : public BaseOp<IFFTShift2DOp<T1>>
  {
  private:
    typename base_type<T1>::type op_;

  public:
    using matxop = bool;
    using scalar_type = typename T1::scalar_type;

    __MATX_INLINE__ IFFTShift2DOp(T1 op) : op_(op) {
      static_assert(Rank() >= 2, "2D IFFT shift must have a rank 2 operator or higher");
    };
    
    template <typename... Is>
    __MATX_INLINE__ __MATX_DEVICE__ __MATX_HOST__ auto operator()(Is... indices) const 
    {
      auto tup = cuda::std::make_tuple(indices...);
      cuda::std::get<Rank()-2>(tup) = (cuda::std::get<Rank()-2>(tup) + (Size(Rank()-2)) / 2) % Size(Rank()-2);
      cuda::std::get<Rank()-1>(tup) = (cuda::std::get<Rank()-1>(tup) + (Size(Rank()-1)) / 2) % Size(Rank()-1);
      return mapply(op_, tup);
    }   

    static __MATX_INLINE__ constexpr __MATX_HOST__ __MATX_DEVICE__ int32_t Rank()
    {
      return detail::get_rank<T1>();
    }
    constexpr __MATX_INLINE__ __MATX_HOST__ __MATX_DEVICE__ auto Size(int dim) const noexcept
    {
      return op_.Size(dim);
    }
  };
  }

  /**
 * Perform an IFFTShift operation on a 2D tensor swapping the first quadrant
 * with the third, and the second with the fourth.
 *
 * Shifts the new indexing of the tensor's last dimension to begin at
 * Size()/2. MatX FFTs leave the sample order starting with DC, positive
 * frequencies, then negative frequencies last. IFFTShift gives a shifted
 * view of a signal where the new order is negative frequencies, DC, then
 * positive frequencies. Note that ifftshift is the same as fftshift if the
 * length of the signal is even.
 *
 * @tparam T1
 *   Type of View/Op
 * @param t
 *   View/Op to shift
 *
 */
  template <typename T1>
  auto ifftshift2D(T1 t) { return detail::IFFTShift2DOp<T1>(t); }

  
  namespace detail {
  template <typename T1>
  class R2COp : public BaseOp<R2COp<T1>>
  {
  private:
    typename base_type<T1>::type op_;
    index_t orig_size_;

  public:
    using matxop = bool;
    using scalar_type = typename T1::scalar_type; 

    __MATX_INLINE__ R2COp(T1 op, index_t orig) : op_(op), orig_size_(orig) {
      static_assert(Rank() >= 1, "R2COp must have a rank 1 operator or higher");
    };

    template <typename... Is>
    __MATX_INLINE__ __MATX_DEVICE__ __MATX_HOST__ auto operator()(Is... indices) const 
    {
      auto tup = cuda::std::make_tuple(indices...);

      // If we're on the upper part of the spectrum, return the conjugate of the first half
      if (cuda::std::get<Rank()-1>(tup) >= op_.Size(Rank()-1)) {
        cuda::std::get<Rank()-1>(tup) = orig_size_ - cuda::std::get<Rank()-1>(tup);
        return cuda::std::conj(mapply(op_, tup));
      }

      return mapply(op_, tup);
    }   

    static __MATX_INLINE__ constexpr __MATX_HOST__ __MATX_DEVICE__ int32_t Rank()
    {
      return detail::get_rank<T1>();
    }
    constexpr __MATX_INLINE__ __MATX_HOST__ __MATX_DEVICE__ auto Size(int dim) const noexcept
    {
      if (dim == (uint32_t)(Rank() - 1)) {
        return orig_size_;
      }
      else {
        return op_.Size(dim);
      }
    }
  };
  }

/**
 * Returns the full spectrum from an R2C transform
 *
 * cuFFT's R2C FFTs only return half the spectrum since the other half is the complex
 * conjugate of the first half. This operator returns the full spectrum from the output
 * of an R2C FFT.
 *
 * @tparam T1
 *   Type of View/Op
 * @param t
 *   View/Op to shift
 * @param orig
 *   Original size. Needed to disambiguate between integer division giving same output size
 *
 */
  template <typename T1>
  auto r2cop(T1 t, index_t orig) { return detail::R2COp<T1>(t, orig); }


  namespace detail {
  template <typename T1>
  class ComplexPlanarOp : public BaseOp<ComplexPlanarOp<T1>>
  {
  private:
    typename base_type<T1>::type op_;

  public:
    using matxop = bool;
    using scalar_type = typename T1::scalar_type;

    __MATX_INLINE__ ComplexPlanarOp(T1 op) : op_(op) {
      static_assert(is_complex_v<extract_scalar_type_t<T1>>, "Complex planar op only works on complex types");
      static_assert(Rank() > 0);
    };

    template <typename... Is>
    __MATX_INLINE__ __MATX_DEVICE__ __MATX_HOST__ auto operator()(Is... indices) const 
    {
      constexpr size_t rank_idx = (Rank() == 1) ? 0 : (Rank() - 2);
      auto tup = cuda::std::make_tuple(indices...);
      if (cuda::std::get<rank_idx>(tup) >= op_.Size(rank_idx)) {      
        cuda::std::get<rank_idx>(tup) -= op_.Size(rank_idx);    
        return mapply(op_, tup).imag();
      }

      return op_(indices...).real();      
    }   

    static __MATX_INLINE__ constexpr __MATX_HOST__ __MATX_DEVICE__ int32_t Rank()
    {
      return detail::get_rank<T1>();
    }
    constexpr __MATX_INLINE__ __MATX_HOST__ __MATX_DEVICE__ auto Size(int dim) const noexcept
    {
      if constexpr (Rank() <= 1)
      {
        return op_.Size(dim) * 2;
      }

      return (dim == static_cast<int>(Rank()) - 2) ? op_.Size(dim) * 2
                                                        : op_.Size(dim);
    }
  };
  }

  /**
   * Perform a planar layout shift on a complex interleaved input
   *
   * Takes an interleaved complex layout (real1, imag1, real2, ...) and transforms
   * it into planar format (real1, real2, ... realN, imag1, ... imagN). This is
   * mostly used for tensor core CGEMM which expects this layout. The indexing on
   * the new layout will be twice as many elements as complex elements since
   * real/imaginary are separated. If the rank is higher than 2, the conversion is
   * treated as a batched transform and only the inner two dims are converted.
   *
   * @tparam T1
   *   Type of View/Op
   * @param t
   *   View/Op to shift
   *
   */
  template <typename T1>
  auto planar(T1 t)
  {
    static_assert(is_complex_v<extract_scalar_type_t<T1>>, "Input to interleaved operator must be complex-valued");
    return detail::ComplexPlanarOp<T1>(t);
  }


  namespace detail {
  template <typename T1>
  class ComplexInterleavedOp : public BaseOp<ComplexInterleavedOp<T1>>
  {
  private:
    typename base_type<T1>::type op_;

  public:
    using matxop = bool;
    using scalar_type = typename T1::scalar_type;

    using complex_type = std::conditional_t<is_matx_half_v<scalar_type>,
                                            matxHalfComplex<scalar_type>,
                                            cuda::std::complex<scalar_type>>;

    __MATX_INLINE__ ComplexInterleavedOp(T1 op) : op_(op) {
      static_assert(!is_complex_v<extract_scalar_type_t<T1>>, "Complex interleaved op only works on scalar input types");
      static_assert(Rank() > 0);      
    };

    template <typename... Is>
    __MATX_INLINE__ __MATX_DEVICE__ __MATX_HOST__ auto operator()(Is... indices) const 
    {
      auto real = op_(indices...);

      constexpr size_t rank_idx = (Rank() == 1) ? 0 : (Rank() - 2);
      auto tup = cuda::std::make_tuple(indices...);
      cuda::std::get<rank_idx>(tup) += op_.Size(rank_idx) / 2;

      auto imag = mapply(op_, tup);
      return complex_type{real, imag};
    } 

    static __MATX_INLINE__ constexpr __MATX_HOST__ __MATX_DEVICE__ int32_t Rank()
    {
      return detail::get_rank<T1>();
    }

    constexpr __MATX_INLINE__ __MATX_HOST__ __MATX_DEVICE__ auto Size(int dim) const noexcept
    {
      if constexpr (Rank() <= 1)
      {
        return op_.Size(dim) / 2;
      }

      return (dim == static_cast<int>(Rank()) - 2) ? op_.Size(dim) / 2
                                                        : op_.Size(dim);
    }
  };
  }

  /**
 * Perform an interleaved layout shift from a complex planar input
 *
 * Takes aplanar complex layout (real1, real2, ... realN, imag1, ... imagN). and
 * transforms it into interleaved format: (real1, imag1, real2, ...). This is
 * mostly used for tensor core CGEMM which expects planar inputs. The indexing
 * on the new layout will be half as many elements as complex elements since
 * real/imaginary are separated in planar. If the rank is higher than 2, the
 * conversion is treated as a batched transform and only the inner two dims are
 * converted.
 *
 * @tparam T1
 *   Type of View/Op
 * @param t
 *   View/Op to shift
 *
 */
  template <typename T1>
  auto interleaved(T1 t)
  {
    static_assert(!is_complex_v<extract_scalar_type_t<T1>>, "Input to interleaved operator must be real-valued");
    return detail::ComplexInterleavedOp<T1>(t);
  }

  namespace detail {
  template <class I1, class Op>
  class matxUnaryOp :  public BaseOp<matxUnaryOp<I1,Op>>
  {
  private:
    typename base_type<I1>::type in1_;
    typename base_type<Op>::type op_;
    std::array<index_t, detail::get_rank<I1>()> size_;

  public:
    // dummy type to signal this is a matxop
    using matxop = bool;
    using scalar_type = typename Op::scalar_type;

    __MATX_INLINE__ matxUnaryOp(I1 in1, Op op) : in1_(in1), op_(op) {
      if constexpr (Rank() > 0) {
        for (int32_t i = 0; i < Rank(); i++) {
          size_[i] = get_size(in1_, i);
        }
      }
    }

    template <typename... Is>
    __MATX_INLINE__ __MATX_DEVICE__ __MATX_HOST__ auto operator()(Is... indices) const 
    {  
      auto i1 = get_value(in1_, indices...);
      return op_(i1);        
    }

    static __MATX_INLINE__ constexpr __MATX_HOST__ __MATX_DEVICE__ int32_t Rank()
    {
      return detail::get_rank<I1>();
    }

    constexpr __MATX_INLINE__ __MATX_HOST__ __MATX_DEVICE__ auto Size(int dim) const noexcept
    {
      return size_[dim];
    }
  };


  template <class I1, class I2, class Op>
  class matxBinaryOp : public BaseOp<matxBinaryOp<I1,I2,Op>>
  {
  private:
    typename base_type<I1>::type in1_;
    typename base_type<I2>::type in2_;
    typename base_type<Op>::type op_;
    std::array<index_t, detail::matx_max(detail::get_rank<I1>(), detail::get_rank<I2>())> size_;

  public:
    // dummy type to signal this is a matxop
    using matxop = bool;
    using scalar_type = typename Op::scalar_type;
    __MATX_INLINE__ matxBinaryOp(I1 in1, I2 in2, Op op) : in1_(in1), in2_(in2), op_(op)
    {
      if constexpr (Rank() > 0)
      {
        for (int32_t i = 0; i < Rank(); i++)
        {
          index_t size1 = detail::get_expanded_size<Rank()>(in1_, i);
          index_t size2 = detail::get_expanded_size<Rank()>(in2_, i);
          size_[i] = detail::matx_max(size1, size2);
          MATX_ASSERT(size1 == 0 || size1 == Size(i), matxInvalidSize);
          MATX_ASSERT(size2 == 0 || size2 == Size(i), matxInvalidSize);
        }
      }
    }

    template <typename... Is>
    __MATX_DEVICE__ __MATX_HOST__ __MATX_INLINE__ auto operator()(Is... indices) const
    {
      // Rank 0
      auto i1 = get_value(in1_, indices...);
      auto i2 = get_value(in2_, indices...);
      return op_(i1, i2);
    }

    static __MATX_INLINE__ constexpr __MATX_HOST__ __MATX_DEVICE__ int32_t Rank()
    {
      return detail::matx_max(detail::get_rank<I1>(), detail::get_rank<I2>());
    }

    constexpr __MATX_INLINE__ __MATX_HOST__ __MATX_DEVICE__ auto Size(int dim) const noexcept
    {
      return size_[dim];
    }
};
}


#define DEFINE_UNARY_OP(FUNCTION, TENSOR_OP)                        \
  template <typename I1,                                            \
            typename = typename std::enable_if_t<is_matx_op<I1>()>> \
  [[nodiscard]] __MATX_INLINE__ auto FUNCTION(I1 i1)                         \
  {                                                                 \
    using I1Type = extract_scalar_type_t<I1>;                       \
    using Op = TENSOR_OP<I1Type>;                                   \
    const typename detail::base_type<I1>::type &base = i1;          \
    return detail::matxUnaryOp(base, Op());                           \
  }

#define DEFINE_BINARY_OP(FUNCTION, TENSOR_OP)                        \
  template <typename I1, typename I2,                                \
            typename = typename std::enable_if_t<is_matx_op<I1>() or \
                                                 is_matx_op<I2>()>>  \
  [[nodiscard]] __MATX_INLINE__ auto FUNCTION(I1 i1, I2 i2)                   \
  {                                                                  \
    using I1Type = extract_scalar_type_t<I1>;                        \
    using I2Type = extract_scalar_type_t<I2>;                        \
    using Op = TENSOR_OP<I1Type, I2Type>;                            \
    const typename detail::base_type<I1>::type &base1 = i1;       \
    const typename detail::base_type<I2>::type &base2 = i2;       \
    return detail::matxBinaryOp(base1, base2, Op());              \
  }

#ifdef DOXYGEN_ONLY
  /**
 * Compute the square root of each value in a tensor.
 * @param t
 *   Tensor or operator input
 */
  Op sqrt(Op t) {}

  /**
 * Compute e^x of each value in a tensor.
 * @param t
 *   Tensor or operator input
 */
  Op exp(Op t) {}

  /**
 * Compute e^(jx) of each value in a tensor where j is sqrt(-1).
 * @param t
 *   Tensor or operator input
 */
  Op expj(Op t) {}

  /**
 * Compute log base 10 of every element in the tensor
 * @param t
 *   Tensor or operator input
 */
  Op log10(Op t) {}

  /**
 * Compute log base 2 of every element in the tensor
 * @param t
 *   Tensor or operator input
 */
  Op log2(Op t) {}

  /**
 * Compute log base e (natural log) of every element in the tensor
 * @param t
 *   Tensor or operator input
 */
  Op log(Op t) {}

  /**
 * Compute log base e (natural log) of every element in the tensor
 * @param t
 *   Tensor or operator input
 */
  Op loge(Op t) {}

  /**
 * Compute the complex conjugate of every element in the tensor
 * @param t
 *   Tensor or operator input
 */
  Op conj(Op t) {}

  /**
 * Compute the squared magnitude of every element in the tensor
 * @param t
 *   Tensor or operator input
 */
  Op norm(Op t) {}

  /**
 * Compute absolute value of every element in the tensor. For complex numbers
 * this returns the magnitude, or sqrt(x^2+y^2)
 * @param t
 *   Tensor or operator input
 */
  Op abs(Op t) {}

  /**
 * Compute the sine of every element in the tensor
 * @param t
 *   Tensor or operator input
 */
  Op sin(Op t) {}

  /**
 * Compute cosine of every element in the tensor
 * @param t
 *   Tensor or operator input
 */
  Op cos(Op t) {}

  /**
 * Compute the tangent of every element in the tensor
 * @param t
 *   Tensor or operator input
 */
  Op tan(Op t) {}

  /**
 * Compute the hyperbolic sine of every element in the tensor
 * @param t
 *   Tensor or operator input
 */
  Op sinh(Op t) {}

  /**
 * Compute hyperbolic cosine of every element in the tensor
 * @param t
 *   Tensor or operator input
 */
  Op cosh(Op t) {}

  /**
 * Compute the hyperbolic tangent of every element in the tensor
 * @param t
 *   Tensor or operator input
 */
  Op tanh(Op t) {}

  /**
 * Compute the arcsine of every element in the tensor
 * @param t
 *   Tensor or operator input
 */
  Op asin(Op t) {}

  /**
 * Compute arccosine of every element in the tensor
 * @param t
 *   Tensor or operator input
 */
  Op acos(Op t) {}

  /**
 * Compute the arctangent of every element in the tensor
 * @param t
 *   Tensor or operator input
 */
  Op atan(Op t) {}

  /**
 * Compute the hyperbolic arcsine of every element in the tensor
 * @param t
 *   Tensor or operator input
 */
  Op asinh(Op t) {}

  /**
 * Compute hyperbolic arccosine of every element in the tensor
 * @param t
 *   Tensor or operator input
 */
  Op acosh(Op t) {}

  /**
 * Compute hyperbolic the arctangent of every element in the tensor
 * @param t
 *   Tensor or operator input
 */
  Op atanh(Op t) {}

  /**
 * Compute the angle of a complex number.
 * @param t
 *   Tensor or operator input
 */
  Op angle(Op t) {}

  /**
 * Compute the principal value of the arctangent of y/x for complex numbers
 * @param t
 *   Tensor or operator input
 */
  Op atan2(Op t) {}

  /**
 * Compute the floor of every element in the tensor
 * @param t
 *   Tensor or operator input
 */
  Op floor(Op t) {}

  /**
 * Compute the ceiling of every element in the tensor
 * @param t
 *   Tensor or operator input
 */
  Op ceil(Op t) {}

  /**
 * Round every element in the tensor
 * @param t
 *   Tensor or operator input
 */
  Op round(Op t) {}

  /**
 * Compute !t (logical NOT) of input tensor or operator
 * @param t
 *   LHS tensor or operator input
 */
  Op operator!(Op t) {}

 /**
 * Negate input tensor or operator
 * @param t
 *   LHS tensor or operator input
 */
  Op operator-(Op t) {}  

  /***** Binary operators ********/

  /**
 * Add two operators or tensors
 * @param t
 *   Tensor or operator input
 * @param t2
 *   RHS second tensor or operator input
 */
  Op operator+(Op t, Op t2) {}

  /**
 * Subtract two operators or tensors
 * @param t
 *   LHS tensor or operator input
 * @param t2
 *   RHS second tensor or operator input
 */
  Op operator-(Op t, Op t2) {}

  /**
 * Multiply two operators or tensors
 * @param t
 *   LHS tensor or operator input
 * @param t2
 *   RHS second tensor or operator input
 */
  Op operator*(Op t, Op t2) {}

  /**
 * Multiply two operators or tensors
 * @param t
 *   LHS tensor or operator input
 * @param t2
 *   RHS second tensor or operator input
 */
  Op mul(Op t, Op t2) {}

  /**
 * Divide two operators or tensors
 * @param t
 *   LHS tensor numerator
 * @param t2
 *   RHS tensor or operator denominator
 */
  Op operator/(Op t, Op t2) {}

  /**
 * Modulo two operators or tensors
 * @param t
 *   LHS tensor or operator input
 * @param t2
 *   RHS second tensor or operator modulus
 */
  Op operator%(Op t, Op t2) {}

  /**
 * Compute the t^t2 of two operators or tensors
 * @param t
 *   LHS tensor or operator input
 * @param t2
 *   RHS tensor or operator power
 */
  Op pow(Op t, Op t2) {}

  /**
 * Compute max(t, t2) of two operators or tensors
 * @param t
 *   LHS tensor or operator input
 * @param t2
 *   RHS tensor or operator input
 */
  Op max(Op t, Op t2) {}

  /**
 * Compute min(t, t2) of two operators or tensors
 * @param t
 *   LHS tensor or operator input
 * @param t2
 *   RHS tensor or operator input
 */
  Op min(Op t, Op t2) {}

  /**
 * Compute t < t2 of two operators or tensors
 * @param t
 *   LHS tensor or operator input
 * @param t2
 *   RHS tensor or operator input
 */
  Op operator<(Op t, Op t2) {}

  /**
 * Compute t > t2 of two operators or tensors
 * @param t
 *   LHS tensor or operator input
 * @param t2
 *   RHS tensor or operator input
 */
  Op operator>(Op t, Op t2) {}

  /**
 * Compute t <= t2 of two operators or tensors
 * @param t
 *   LHS tensor or operator input
 * @param t2
 *   RHS tensor or operator input
 */
  Op operator<=(Op t, Op t2) {}

  /**
 * Compute t >= t2 of two operators or tensors
 * @param t
 *   LHS tensor or operator input
 * @param t2
 *   RHS tensor or operator input
 */
  Op operator>=(Op t, Op t2) {}

  /**
 * Compute t == t2 of two operators or tensors
 * @param t
 *   LHS tensor or operator input
 * @param t2
 *   RHS tensor or operator input
 */
  Op operator==(Op t, Op t2) {}

  /**
 * Compute t != t2 of two operators or tensors
 * @param t
 *   LHS tensor or operator input
 * @param t2
 *   RHS tensor or operator input
 */
  Op operator!=(Op t, Op t2) {}

  /**
 * Compute t && t2 (logical AND) of two operators or tensors
 * @param t
 *   LHS tensor or operator input
 * @param t2
 *   RHS tensor or operator input
 */
  Op operator&&(Op t, Op t2) {}

  /**
 * Compute t || t2 (logical OR) of two operators or tensors
 * @param t
 *   LHS tensor or operator input
 * @param t2
 *   RHS tensor or operator input
 */
  Op operator||(Op t, Op t2) {}
#else
  DEFINE_UNARY_OP(sqrt, detail::SqrtOp);
  DEFINE_UNARY_OP(exp, detail::ExpOp);
  DEFINE_UNARY_OP(expj, detail::ExpjOp);
  DEFINE_UNARY_OP(log10, detail::Log10Op);
  DEFINE_UNARY_OP(log2, detail::Log2Op);
  DEFINE_UNARY_OP(log, detail::LogOp);
  DEFINE_UNARY_OP(loge, detail::LogOp);
  DEFINE_UNARY_OP(conj, detail::ConjOp);
  DEFINE_UNARY_OP(norm, detail::NormOp);
  DEFINE_UNARY_OP(abs, detail::AbsOp);
  DEFINE_UNARY_OP(sin, detail::SinOp);
  DEFINE_UNARY_OP(cos, detail::CosOp);
  DEFINE_UNARY_OP(tan, detail::TanOp);
  DEFINE_UNARY_OP(asin, detail::AsinOp);
  DEFINE_UNARY_OP(acos, detail::AcosOp);
  DEFINE_UNARY_OP(atan, detail::AtanOp);
  DEFINE_UNARY_OP(sinh, detail::SinhOp);
  DEFINE_UNARY_OP(cosh, detail::CoshOp);
  DEFINE_UNARY_OP(tanh, detail::TanhOp);
  DEFINE_UNARY_OP(asinh, detail::AsinhOp);
  DEFINE_UNARY_OP(acosh, detail::AcoshOp);
  DEFINE_UNARY_OP(atanh, detail::AtanhOp);
  DEFINE_UNARY_OP(angle, detail::AngleOp);
  DEFINE_UNARY_OP(atan2, detail::AngleOp);
  DEFINE_UNARY_OP(floor, detail::FloorOp);
  DEFINE_UNARY_OP(ceil, detail::CeilOp);
  DEFINE_UNARY_OP(round, detail::RoundOp);
  DEFINE_UNARY_OP(normcdf, detail::NormCdfOp);
  DEFINE_UNARY_OP(real, detail::RealOp);
  DEFINE_UNARY_OP(imag, detail::ImagOp);  
  DEFINE_UNARY_OP(operator-, detail::SubNegOp );

  DEFINE_BINARY_OP(operator+, detail::AddOp);
  DEFINE_BINARY_OP(operator-, detail::SubOp);
  DEFINE_BINARY_OP(operator*, detail::MulOp);
  DEFINE_BINARY_OP(mul, detail::MulOp);
  DEFINE_BINARY_OP(operator/, detail::DivOp);
  DEFINE_BINARY_OP(operator%, detail::ModOp);
  DEFINE_BINARY_OP(fmod, detail::FModOp);
  DEFINE_BINARY_OP(operator|, detail::OrOp);
  DEFINE_BINARY_OP(operator&, detail::AndOp);
  DEFINE_BINARY_OP(operator^, detail::XorOp);
  DEFINE_BINARY_OP(pow, detail::PowOp);
  DEFINE_BINARY_OP(max, detail::MaxOp);
  DEFINE_BINARY_OP(min, detail::MinOp);
  DEFINE_BINARY_OP(operator<, detail::LTOp);
  DEFINE_BINARY_OP(operator>, detail::GTOp);
  DEFINE_BINARY_OP(operator<=, detail::LTEOp);
  DEFINE_BINARY_OP(operator>=, detail::GTEOp);
  DEFINE_BINARY_OP(operator==, detail::EQOp);
  DEFINE_BINARY_OP(operator!=, detail::NEOp);
  DEFINE_BINARY_OP(operator&&, detail::AndAndOp);
  DEFINE_BINARY_OP(operator||, detail::OrOrOp);
  DEFINE_UNARY_OP(operator!, detail::NotOp);
#endif

  // Doxygen doesn't recognize macros generating functions, so we need to fake
  // each one here

} // end namespace matx
