#include "ViewOpToLLVM.h"
#include "DotOpHelpers.h"

using namespace mlir;
using namespace mlir::triton;

using ::mlir::LLVM::DotOpFMAConversionHelper;
using ::mlir::LLVM::DotOpMmaV1ConversionHelper;
using ::mlir::LLVM::DotOpMmaV2ConversionHelper;
using ::mlir::LLVM::getElementsFromStruct;
using ::mlir::LLVM::getSharedMemoryObjectFromStruct;
using ::mlir::LLVM::getStructFromElements;
using ::mlir::LLVM::MMA16816ConversionHelper;
using ::mlir::triton::gpu::getElemsPerThread;

struct SplatOpConversion
    : public ConvertTritonGPUOpToLLVMPattern<triton::SplatOp> {
  using ConvertTritonGPUOpToLLVMPattern<
      triton::SplatOp>::ConvertTritonGPUOpToLLVMPattern;

  // Convert SplatOp or arith::ConstantOp with SplatElementsAttr to a
  // LLVM::StructType value.
  //
  // @elemType: the element type in operand.
  // @resType: the return type of the Splat-like op.
  // @constVal: a LLVM::ConstantOp or other scalar value.
  static Value convertSplatLikeOp(Type elemType, Type resType, Value constVal,
                                  TypeConverter *typeConverter,
                                  ConversionPatternRewriter &rewriter,
                                  Location loc) {
    auto tensorTy = resType.cast<RankedTensorType>();
    auto srcType = typeConverter->convertType(elemType);
    auto llSrc = bitcast(constVal, srcType);
    size_t elemsPerThread = getElemsPerThread(tensorTy);
    llvm::SmallVector<Value> elems(elemsPerThread, llSrc);
    llvm::SmallVector<Type> elemTypes(elems.size(), srcType);
    auto structTy =
        LLVM::LLVMStructType::getLiteral(rewriter.getContext(), elemTypes);

    return getStructFromElements(loc, elems, rewriter, structTy);
  }

  LogicalResult matchAndRewrite(triton::SplatOp op, OpAdaptor adaptor,
                                ConversionPatternRewriter &rewriter) const {
    auto loc = op->getLoc();
    auto src = adaptor.src();
    auto llStruct = convertSplatLikeOp(src.getType(), op.getType(), src,
                                       getTypeConverter(), rewriter, loc);
    rewriter.replaceOp(op, {llStruct});
    return success();
  }
};

// This pattern helps to convert arith::ConstantOp(with SplatElementsAttr),
// the logic is the same as triton::SplatOp, so the underlying implementation
// is reused.
struct ArithConstantSplatOpConversion
    : public ConvertTritonGPUOpToLLVMPattern<arith::ConstantOp> {
  using ConvertTritonGPUOpToLLVMPattern<
      arith::ConstantOp>::ConvertTritonGPUOpToLLVMPattern;

  LogicalResult
  matchAndRewrite(arith::ConstantOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto value = op.getValue();
    if (!value.dyn_cast<SplatElementsAttr>())
      return failure();

    auto loc = op->getLoc();

    LLVM::ConstantOp arithConstantOp;
    auto values = op.getValue().dyn_cast<SplatElementsAttr>();
    auto elemType = values.getElementType();

    Attribute val;
    if (elemType.isBF16() || type::isFloat(elemType)) {
      val = values.getValues<FloatAttr>()[0];
    } else if (type::isInt(elemType)) {
      val = values.getValues<IntegerAttr>()[0];
    } else {
      llvm::errs() << "ArithConstantSplatOpConversion get unsupported type: "
                   << value.getType() << "\n";
      return failure();
    }

    auto constOp = rewriter.create<LLVM::ConstantOp>(loc, elemType, val);
    auto llStruct = SplatOpConversion::convertSplatLikeOp(
        elemType, op.getType(), constOp, getTypeConverter(), rewriter, loc);
    rewriter.replaceOp(op, llStruct);

    return success();
  }
};

struct CatOpConversion : public ConvertTritonGPUOpToLLVMPattern<CatOp> {
  using OpAdaptor = typename CatOp::Adaptor;

  explicit CatOpConversion(LLVMTypeConverter &typeConverter,
                           PatternBenefit benefit = 1)
      : ConvertTritonGPUOpToLLVMPattern<CatOp>(typeConverter, benefit) {}

  LogicalResult
  matchAndRewrite(CatOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Location loc = op->getLoc();
    auto resultTy = op.getType().template cast<RankedTensorType>();
    unsigned elems = getElemsPerThread(resultTy);
    Type elemTy =
        this->getTypeConverter()->convertType(resultTy.getElementType());
    SmallVector<Type> types(elems, elemTy);
    // unpack input values
    auto lhsVals = getElementsFromStruct(loc, adaptor.lhs(), rewriter);
    auto rhsVals = getElementsFromStruct(loc, adaptor.rhs(), rewriter);
    // concatenate (and potentially reorder) values
    SmallVector<Value> retVals;
    for (Value v : lhsVals)
      retVals.push_back(v);
    for (Value v : rhsVals)
      retVals.push_back(v);
    // pack and replace
    Type structTy = LLVM::LLVMStructType::getLiteral(this->getContext(), types);
    Value ret = getStructFromElements(loc, retVals, rewriter, structTy);
    rewriter.replaceOp(op, ret);
    return success();
  }
};

template <typename SourceOp>
struct ViewLikeOpConversion : public ConvertTritonGPUOpToLLVMPattern<SourceOp> {
  using OpAdaptor = typename SourceOp::Adaptor;
  explicit ViewLikeOpConversion(LLVMTypeConverter &typeConverter,
                                PatternBenefit benefit = 1)
      : ConvertTritonGPUOpToLLVMPattern<SourceOp>(typeConverter, benefit) {}

  LogicalResult
  matchAndRewrite(SourceOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    // We cannot directly run `rewriter.replaceOp(op, adaptor.src())`
    // due to MLIR's restrictions
    Location loc = op->getLoc();
    auto resultTy = op.getType().template cast<RankedTensorType>();
    unsigned elems = getElemsPerThread(resultTy);
    Type elemTy =
        this->getTypeConverter()->convertType(resultTy.getElementType());
    SmallVector<Type> types(elems, elemTy);
    Type structTy = LLVM::LLVMStructType::getLiteral(this->getContext(), types);
    auto vals = getElementsFromStruct(loc, adaptor.src(), rewriter);

    Value view = getStructFromElements(loc, vals, rewriter, structTy);
    rewriter.replaceOp(op, view);
    return success();
  }
};

struct TransOpConversion
    : public ConvertTritonGPUOpToLLVMPattern<triton::TransOp> {
  using ConvertTritonGPUOpToLLVMPattern<
      triton::TransOp>::ConvertTritonGPUOpToLLVMPattern;

  LogicalResult
  matchAndRewrite(triton::TransOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Location loc = op->getLoc();
    auto srcSmemObj =
        getSharedMemoryObjectFromStruct(loc, adaptor.src(), rewriter);
    SmallVector<Value> dstStrides = {srcSmemObj.strides[1],
                                     srcSmemObj.strides[0]};
    SmallVector<Value> dstOffsets = {srcSmemObj.offsets[1],
                                     srcSmemObj.offsets[0]};
    auto dstSmemObj =
        SharedMemoryObject(srcSmemObj.base, dstStrides, dstOffsets);
    auto retVal = getStructFromSharedMemoryObject(loc, dstSmemObj, rewriter);
    rewriter.replaceOp(op, retVal);
    return success();
  }
};

void populateViewOpToLLVMPatterns(mlir::LLVMTypeConverter &typeConverter,
                                  RewritePatternSet &patterns, int numWarps,
                                  AxisInfoAnalysis &axisInfoAnalysis,
                                  const Allocation *allocation, Value smem,
                                  PatternBenefit benefit) {
  patterns.add<ViewLikeOpConversion<triton::ViewOp>>(typeConverter, benefit);
  patterns.add<ViewLikeOpConversion<triton::ExpandDimsOp>>(typeConverter,
                                                           benefit);
  patterns.add<SplatOpConversion>(typeConverter, benefit);
  patterns.add<ArithConstantSplatOpConversion>(typeConverter, benefit);
  patterns.add<CatOpConversion>(typeConverter, benefit);
  patterns.add<TransOpConversion>(typeConverter, benefit);
}
