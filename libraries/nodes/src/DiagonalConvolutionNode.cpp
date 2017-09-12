////////////////////////////////////////////////////////////////////////////////////////////////////
//
//  Project:  Embedded Learning Library (ELL)
//  File:     DiagonalConvolutionNode.cpp (nodes)
//  Authors:  Chuck Jacobs
//
////////////////////////////////////////////////////////////////////////////////////////////////////

#include "DiagonalConvolutionNode.h"
#include "ConstantNode.h"
#include "MatrixMatrixMultiplyNode.h"

using namespace ell::math;
using namespace ell::math::Blas;

namespace ell
{
namespace nodes
{
    namespace
    {
        // Useful aliases for operators
        const auto plus = emitters::TypedOperator::add;
        const auto minus = emitters::TypedOperator::subtract;
        const auto times = emitters::TypedOperator::multiply;
        const auto divide = emitters::TypedOperator::divideSigned;
        const auto modulo = emitters::TypedOperator::moduloSigned;

        const auto plusFloat = emitters::TypedOperator::addFloat;
        const auto minusFloat = emitters::TypedOperator::subtractFloat;
        const auto timesFloat = emitters::TypedOperator::multiplyFloat;
        const auto divideFloat = emitters::TypedOperator::divideFloat;

        const auto logicalOr = emitters::TypedOperator::logicalOr;
        const auto logicalAnd = emitters::TypedOperator::logicalAnd;
        const auto shiftLeft = emitters::TypedOperator::shiftLeft;

        // comparisons
        const auto lessThan = emitters::TypedComparison::lessThan;
        const auto greaterThanOrEqual = emitters::TypedComparison::greaterThanOrEquals;
        const auto greaterThanFloat = emitters::TypedComparison::greaterThanFloat;

        //
        // Functions to emit portions of IR
        //

        template <typename ValueType>
        void EmitMatrixMatrixMultiplyBlas(emitters::IRFunctionEmitter& function, bool transposeA, bool transposeB, int m, int n, int k, llvm::Value* A, int lda, llvm::Value* B, int ldb, llvm::Value* C, int ldc)
        {
            llvm::Function* gemm = function.GetModule().GetRuntime().GetGEMMFunction<ValueType>();

            emitters::IRValueList args{
                function.Literal(GetCBlasMatrixOrder(MatrixLayout::rowMajor)), // order
                function.Literal(GetCBlasMatrixTranspose(transposeA ? MatrixTranspose::transpose : MatrixTranspose::noTranspose)), // transposeA
                function.Literal(GetCBlasMatrixTranspose(transposeB ? MatrixTranspose::transpose : MatrixTranspose::noTranspose)), // transposeB
                function.Literal(m),
                function.Literal(n),
                function.Literal(k),
                function.Literal(static_cast<ValueType>(1.0)), // alpha
                A,
                function.Literal(lda), // lda
                B,
                function.Literal(ldb), // ldb
                function.Literal(static_cast<ValueType>(0.0)), // beta
                C, // C (output)
                function.Literal(ldc) // ldc
            };
            function.Call(gemm, args);
        }

        // TODO: emit this as a function in the module
        template <typename ValueType>
        void EmitMatrixMatrixMultiplySlow(emitters::IRFunctionEmitter& function, bool transposeA, bool transposeB, int m, int n, int k, llvm::Value* A, int lda, llvm::Value* B, int ldb, llvm::Value* C, int ldc)
        {
            if (transposeA || transposeB)
            {
                assert(false && "Transpose not implemented in slow mat mult path");
            }

            llvm::Value* accum = function.Variable(emitters::GetVariableType<ValueType>(), "accum");

            auto mLoop = function.ForLoop();
            mLoop.Begin(m);
            {
                auto mIndex = mLoop.LoadIterationVariable();

                auto nLoop = function.ForLoop();
                nLoop.Begin(n);
                {
                    auto nIndex = nLoop.LoadIterationVariable();

                    function.Store(accum, function.Literal(static_cast<ValueType>(0.0)));
                    auto kLoop = function.ForLoop();
                    kLoop.Begin(k);
                    {
                        auto kIndex = kLoop.LoadIterationVariable();

                        auto aIndex = function.Operator(plus, function.Operator(times, mIndex, function.Literal(lda)), kIndex);
                        auto aValue = function.ValueAt(A, aIndex);
                        auto bIndex = function.Operator(plus, function.Operator(times, kIndex, function.Literal(ldb)), nIndex);
                        auto bValue = function.ValueAt(B, bIndex);
                        auto value = function.Operator(timesFloat, aValue, bValue);
                        function.OperationAndUpdate(accum, plusFloat, value);
                    }
                    kLoop.End();

                    // store output in C[m,n]
                    auto cIndex = function.Operator(plus, function.Operator(times, mIndex, function.Literal(ldc)), nIndex);
                    function.SetValueAt(C, cIndex, function.Load(accum));
                }
                nLoop.End();
            }
            mLoop.End();
        }

        template <typename ValueType>
        void EmitMatrixMatrixMultiply(emitters::IRFunctionEmitter& function, bool useBlas, bool transposeA, bool transposeB, int m, int n, int k, llvm::Value* A, int lda, llvm::Value* B, int ldb, llvm::Value* C, int ldc)
        {
            if (useBlas)
            {
                EmitMatrixMatrixMultiplyBlas<ValueType>(function, transposeA, transposeB, m, n, k, A, lda, B, ldb, C, ldc);
            }
            else
            {
                EmitMatrixMatrixMultiplySlow<ValueType>(function, transposeA, transposeB, m, n, k, A, lda, B, ldb, C, ldc);
            }
        }

        size_t GetFilterVolumeSize(const predictors::neural::ConvolutionalParameters& convolutionalParameters, const model::PortMemoryLayout& inputLayout)
        {
            const auto inputDepth = inputLayout.GetActiveSize(2);
            const auto filterWidth = convolutionalParameters.receptiveField;
            return inputDepth * filterWidth * filterWidth;
        }

        size_t GetDiagonalConvolutionOutputSize(const model::PortMemoryLayout& outputLayout)
        {
            return outputLayout.GetActiveSize(0) * outputLayout.GetActiveSize(1) * outputLayout.GetActiveSize(2);
        }
    } // end anonymous namespace

    //
    // DiagonalConvolutionNode
    //

    template <typename ValueType>
    DiagonalConvolutionNode<ValueType>::DiagonalConvolutionNode()
        : CompilableNode({ &_input }, { &_output }), _input(this, {}, inputPortName), _filterWeights(this, {}, filterWeightsPortName), _output(this, outputPortName, 0)
    {
    }

    template <typename ValueType>
    DiagonalConvolutionNode<ValueType>::DiagonalConvolutionNode(const model::PortElements<ValueType>& input, const model::PortMemoryLayout& inputMemoryLayout, const model::PortElements<ValueType>& filterWeights, const model::PortMemoryLayout& outputMemoryLayout, const predictors::neural::ConvolutionalParameters& convolutionalParameters)
        : CompilableNode({ &_input, &_filterWeights }, { &_output }), _input(this, input, inputPortName), _filterWeights(this, filterWeights, filterWeightsPortName), _output(this, outputPortName, GetDiagonalConvolutionOutputSize(outputMemoryLayout)), _inputMemoryLayout(inputMemoryLayout), _outputMemoryLayout(outputMemoryLayout), _convolutionalParameters(convolutionalParameters)
    {
    }

    template <typename ValueType>
    void DiagonalConvolutionNode<ValueType>::Copy(model::ModelTransformer& transformer) const
    {
        auto newInput = transformer.TransformPortElements(_input.GetPortElements());
        auto newFilterWeights = transformer.TransformPortElements(_filterWeights.GetPortElements());
        auto newNode = transformer.AddNode<DiagonalConvolutionNode<ValueType>>(newInput, _inputMemoryLayout, newFilterWeights, _outputMemoryLayout, _convolutionalParameters);
        transformer.MapNodeOutput(this->output, newNode->output);
    }

    template <typename ValueType>
    void DiagonalConvolutionNode<ValueType>::Compute() const
    {
        // TODO: Deal with pre-padded input

        // Model parameters
        auto&& inputLayout = this->GetInputMemoryLayout();
        auto&& outputLayout = this->GetOutputMemoryLayout();
        auto&& convParams = this->GetConvolutionalParameters();
        const auto inputHeight = inputLayout.GetActiveSize(0);
        const auto inputWidth = inputLayout.GetActiveSize(1);
        const auto inputDepth = inputLayout.GetActiveSize(2);
        const auto inputPadding = inputLayout.GetOffset(0);
        const auto filterWidth = convParams.receptiveField;
        const auto fieldVolumeSize = filterWidth * filterWidth * inputDepth;

        const auto outputWidth = outputLayout.GetActiveSize(1);
        const auto outputHeight = outputLayout.GetActiveSize(0);
        const auto numFilters = outputLayout.GetActiveSize(2);
        const auto outputPadding = outputLayout.GetOffset(0);

        const auto padding = inputPadding; // ?

        // const size_t batchSize = _convolutionalParameters.numFiltersAtATime;
        const size_t batchSize = numFilters;

        const size_t paddedWidth = outputWidth + (2 * padding);
        const size_t paddedHeight = outputHeight + (2 * padding);
        assert(_input.Size() == paddedWidth * paddedHeight * inputDepth);

        const size_t numFlattenedMatrixColumns = inputDepth * paddedWidth;

        const size_t kd = filterWidth * inputDepth;
        // const size_t numConvolutions = ((paddedWidth - filterWidth) * inputDepth + 1) / inputDepth; // Note: this should be paddedWidth - filterWidth + 1?
        const size_t numConvolutions = paddedWidth - filterWidth + 1; // Note: numConvolutions == output width if padding is on

        auto inputData = _input.GetValue();
        auto filterWeightsData = _filterWeights.GetValue();
        assert(filterWeightsData.size() == filterWidth * filterWidth * inputDepth * numFilters);
        const size_t outputSize = (paddedHeight * paddedWidth) * numFilters;
        std::vector<ValueType> output(outputSize);
        std::vector<ValueType> scratch(paddedHeight * filterWidth * batchSize);

        math::RowMatrixReference<ValueType> inputMatrix(paddedHeight, numFlattenedMatrixColumns, inputData.data());
        math::RowMatrixReference<ValueType> weightsMatrix(filterWidth * inputDepth, filterWidth * numFilters, filterWeightsData.data());
        math::RowMatrixReference<ValueType> outputMatrix(paddedHeight, paddedWidth * numFilters, output.data()); //
        auto scratchData = scratch.data();

        for (size_t j = 0; j < numConvolutions; j++) // each pass through this loop computes 1 column of the output image, for all filters
        {
            auto Vj = inputMatrix.GetSubMatrix(0, j * inputDepth, inputMatrix.NumRows(), kd);
            for (size_t filterStart = 0; filterStart < numFilters; filterStart += batchSize)
            {
                size_t numFiltersToUse = std::min(batchSize, numFilters - filterStart);

                auto Wl = weightsMatrix.GetSubMatrix(0, filterStart * filterWidth, weightsMatrix.NumRows(), numFiltersToUse * filterWidth);
                math::RowMatrixReference<ValueType> A(Vj.NumRows(), Wl.NumColumns(), scratch.data());

                math::Multiply(static_cast<ValueType>(1.0), Vj, Wl, static_cast<ValueType>(0.0), A);

                for (size_t l = 0; l < numFiltersToUse; l++)
                {
                    for (size_t startRow = 0; startRow < (paddedHeight - 2 * padding); startRow++) // assumes padding = floor(filterWidth/2)
                    {
                        double sum = 0.0;
                        for (size_t diagonal = 0; diagonal < filterWidth; diagonal++)
                        {
                            sum += A(startRow + diagonal, l * filterWidth + diagonal);
                        }

                        outputMatrix(startRow + padding, j + padding + paddedWidth * (filterStart + l)) = sum;
                    }
                }
            }
        }

        _output.SetOutput(output);
    }

    template <typename ValueType>
    void DiagonalConvolutionNode<ValueType>::Compile(model::IRMapCompiler& compiler, emitters::IRFunctionEmitter& function)
    {
        auto& context = function.GetModule().GetLLVMContext();
        auto voidType = llvm::Type::getVoidTy(context);
        auto int8Type = llvm::Type::getInt8Ty(context);
        auto voidPtrType = int8Type->getPointerTo();
        auto nullValue = llvm::ConstantPointerNull::get(voidPtrType);

        // input is a d x (w+2p) x (h+2p) array
        // reshaped, it's a d*(w+2p)) x (h+2p) array == d*(w+k-1) x (h+k-1)
        llvm::Value* pInput = compiler.EnsurePortEmitted(this->input);

        // weights is f x k x k x d array
        // reshaped, it's (f*k) x (k*d) or f x k x (k*d)
        llvm::Value* pWeights = compiler.EnsurePortEmitted(this->filterWeights);

        // output is a (w+2p) x (h+2p) x f array
        llvm::Value* pOutput = compiler.EnsurePortEmitted(this->output);

        const bool useBlas = compiler.GetMapCompilerParameters().compilerSettings.useBlas;

        // Model parameters
        auto&& inputLayout = this->GetInputMemoryLayout();
        auto&& outputLayout = this->GetOutputMemoryLayout();
        auto&& convParams = this->GetConvolutionalParameters();
        const auto inputHeight = inputLayout.GetActiveSize(0);
        const auto inputWidth = inputLayout.GetActiveSize(1);
        const auto inputDepth = inputLayout.GetActiveSize(2);
        const auto inputPadding = inputLayout.GetOffset(0);
        const auto filterWidth = convParams.receptiveField;
        const auto fieldVolumeSize = filterWidth * filterWidth * inputDepth;
        const auto padding = inputLayout.GetOffset(0);
        assert((padding == filterWidth / 2) && "Padding must be filterWidth/2");

        // input data parameters
        // const size_t paddedWidth = inputWidth + (2 * padding);
        // const size_t paddedHeight = inputHeight + (2 * padding);
        const size_t paddedWidth = inputLayout.GetStride(1);
        const size_t paddedHeight = inputLayout.GetStride(0);
        const size_t numFlattenedMatrixColumns = inputDepth * paddedWidth;

        const auto outputWidth = outputLayout.GetActiveSize(1);
        const auto outputHeight = outputLayout.GetActiveSize(0);
        const auto numFilters = outputLayout.GetActiveSize(2);
        const auto outputPadding = outputLayout.GetOffset(0);

        // output data parameters
        const size_t outputSize = outputWidth * outputHeight * numFilters;

        // computation parameters
        const size_t batchSize = numFilters;
        const size_t stackSize = inputWidth;
        // const size_t stackSize = 1;

        // TODO: check this carefully to make sure it's valid for stackSize != all and stackSize != 1
        const size_t stackedInputHeight = (inputHeight + padding) * stackSize + padding;
        const size_t columnsPerStack = (inputWidth - 1) / stackSize + 1;
        const size_t stackedInputWidth = (columnsPerStack + 2 * padding);
        const size_t stackedInputSize = stackedInputWidth * stackedInputHeight * inputDepth;

        llvm::Value* pStackedInput = nullptr;
        const size_t inputStride = paddedWidth * inputDepth;
        const size_t stackedInputStride = stackedInputWidth * inputDepth;
        if (stackSize != 1)
        {
            llvm::GlobalVariable* stackedInput = function.GetModule().GlobalArray(emitters::GetVariableType<ValueType>(), "stackedInput", stackedInputSize);
            pStackedInput = function.PointerOffset(stackedInput, 0); // convert "global variable" to a pointer

            // Fill in input memory
            // First, the top p rows of zeros (padding):
            for (int rowIndex = 0; rowIndex < padding; ++rowIndex)
            {
                function.MemoryCopy<ValueType>(pInput, rowIndex * inputStride, pStackedInput, rowIndex * stackedInputStride, stackedInputStride);
            }

            // Now skip past the first p rows of padding and copy the rest
            auto inputPtr = function.PointerOffset(pInput, padding * inputStride);
            auto stackedInputPtr = function.PointerOffset(pStackedInput, padding * stackedInputStride);
            auto stackLoop = function.ForLoop();
            stackLoop.Begin(stackSize); // foreach stack
            {
                auto stackIndex = stackLoop.LoadIterationVariable();
                auto inputColOffset = function.Operator(times, stackIndex, function.Literal<int>(columnsPerStack * inputDepth));
                auto stackBeginOffset = function.Operator(times, stackIndex, function.Literal<int>((inputHeight + padding) * stackedInputStride));
                auto copyLoop = function.ForLoop();
                copyLoop.Begin(inputHeight + padding); // foreach row
                {
                    auto rowIndex = copyLoop.LoadIterationVariable();
                    auto inputOffset = function.Operator(plus, inputColOffset, function.Operator(times, rowIndex, function.Literal<int>(inputStride)));
                    auto outputOffset = function.Operator(plus, stackBeginOffset, function.Operator(times, rowIndex, function.Literal<int>(stackedInputStride)));
                    function.MemoryCopy<ValueType>(inputPtr, inputOffset, stackedInputPtr, outputOffset, function.Literal<int>(stackedInputStride));
                }
                copyLoop.End();
            }
            stackLoop.End();
        }
        else
        {
            pStackedInput = pInput;
        }

        // Allocate scratch memory for 'A' matrix
        // TODO: this is really paddedHeight * filterWidth * batchSize * stackSize - padding * filterWidth * batchSize
        //              == (inputHeight + padding) * filterWidth * batchSize * (stackSize + padding);
        const size_t scratchMemSize = paddedHeight * filterWidth * batchSize * stackSize;
        llvm::GlobalVariable* scratch = function.GetModule().GlobalArray(emitters::GetVariableType<ValueType>(), "scratch", scratchMemSize);
        auto scratchPtr = function.PointerOffset(scratch, 0); // Convert LLVM array to pointer

        const int outputStride = paddedWidth * numFilters;
        const size_t numConvolutions = (inputWidth - 1) / stackSize + 1;
        auto convLoop = function.ForLoop();
        convLoop.Begin(numConvolutions);
        {
            auto j = convLoop.LoadIterationVariable(); // j = start column for convolution

            // Get the submatrix for Vj
            auto inputOffset = function.Operator(times, j, function.Literal<int>(inputDepth));
            llvm::Value* Vj = function.PointerOffset(pStackedInput, inputOffset);

            // now for each batch of filter weights
            for (size_t filterStart = 0; filterStart < numFilters; filterStart += batchSize)
            {
                size_t numFiltersToUse = std::min(batchSize, numFilters - filterStart);

                // Get the submatrix for Wl
                auto weightsOffset = filterStart * filterWidth;
                llvm::Value* Wl = function.PointerOffset(pWeights, weightsOffset);

                // int m = paddedHeight;
                int m = stackedInputHeight;
                int n = filterWidth * numFiltersToUse; // this batch
                int k = inputDepth * filterWidth;
                int lda = stackedInputWidth * inputDepth;
                int ldb = filterWidth * numFilters;
                int ldc = filterWidth * batchSize;

                // Note: Wl is transposed
                EmitMatrixMatrixMultiply<ValueType>(function, useBlas, false, true, m, n, k, Vj, lda, Wl, ldb, scratchPtr, ldc);

                // S loop here as well
                auto stackLoop = function.ForLoop();
                stackLoop.Begin(stackSize);
                {
                    auto stackIndex = stackLoop.LoadIterationVariable();
                    auto stackRowOffset = function.Operator(times, stackIndex, function.Literal<int>(inputHeight + padding));
                    auto outputColumn = function.Operator(plus, j, function.Operator(times, stackIndex, function.Literal<int>(numConvolutions)));

                    auto lLoop = function.ForLoop();
                    lLoop.Begin(numFiltersToUse);
                    {
                        auto l = lLoop.LoadIterationVariable(); // batchFilterIndex
                        auto filterIndex = function.Operator(plus, function.Literal<int>(filterStart), l);
                        auto startRowLoop = function.ForLoop();
                        startRowLoop.Begin(inputHeight);
                        {
                            auto startRow = startRowLoop.LoadIterationVariable();
                            auto stackStartRow = function.Operator(plus, stackRowOffset, startRow);
                            llvm::Value* sum = nullptr;
                            for (size_t diagonal = 0; diagonal < filterWidth; diagonal++)
                            {
                                auto currRow = function.Operator(plus, stackStartRow, function.Literal<int>(diagonal));
                                auto currRowOffset = function.Operator(times, currRow, function.Literal<int>(batchSize * filterWidth));
                                // auto currColOffset = function.Operator(plus, function.Literal<int>(batchSize * diagonal), l);
                                // col offset = l*k + diagonal
                                auto currColOffset = function.Operator(plus, function.Operator(times, l, function.Literal<int>(filterWidth)), function.Literal<int>(diagonal));

                                auto inputIndex = function.Operator(plus, currRowOffset, currColOffset);
                                llvm::Value* diagonalValue = function.ValueAt(scratchPtr, inputIndex);
                                // diagonalValue = A[startRow + diagonal, l*k + diagonal]
                                if (sum == nullptr)
                                    sum = diagonalValue;
                                else
                                    sum = function.Operator(plusFloat, sum, diagonalValue);
                            }
                            auto outRowOffset = function.Operator(times, startRow, function.Literal<int>(outputStride));
                            auto outColOffset = function.Operator(times, outputColumn, function.Literal<int>(numFilters));
                            auto outputIndex = function.Operator(plus, function.Operator(plus, outRowOffset, outColOffset), filterIndex);
                            function.SetValueAt(pOutput, outputIndex, sum);
                        }
                        startRowLoop.End();
                    }
                    lLoop.End();
                }
                stackLoop.End();
            }
        }
        convLoop.End();
    }

    // Explicit specializations
    template class DiagonalConvolutionNode<float>;
    template class DiagonalConvolutionNode<double>;
} // nodes
} // ell
