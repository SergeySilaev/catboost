#pragma once

#include "model.h"
#include <catboost/libs/helpers/exception.h>


constexpr size_t FORMULA_EVALUATION_BLOCK_SIZE = 128;

inline void OneHotBinsFromTransposedCatFeatures(
    const TVector<TOneHotFeature>& OneHotFeatures,
    const THashMap<int, int> catFeaturePackedIndex,
    const size_t docCount,
    ui8*& result,
    TVector<int>& transposedHash) {
    for (const auto& oheFeature : OneHotFeatures) {
        const auto catIdx = catFeaturePackedIndex.at(oheFeature.CatFeatureIndex);
        for (size_t docId = 0; docId < docCount; ++docId) {
            const auto val = transposedHash[catIdx * docCount + docId];
            for (size_t borderIdx = 0; borderIdx < oheFeature.Values.size(); ++borderIdx) {
                result[docId] |= (ui8)(val == oheFeature.Values[borderIdx]) * (borderIdx + 1);
            }
        }
        result += docCount;
    }
}

template<typename TFloatFeatureAccessor>
Y_FORCE_INLINE void BinarizeFloats(const size_t docCount, TFloatFeatureAccessor floatAccessor, const TConstArrayRef<float> borders, size_t start, ui8*& result) {
    const auto docCount8 = (docCount | 0x7) ^ 0x7;
    for (size_t docId = 0; docId < docCount8; docId += 8) {
        const float val[8] = {
            floatAccessor(start + docId + 0),
            floatAccessor(start + docId + 1),
            floatAccessor(start + docId + 2),
            floatAccessor(start + docId + 3),
            floatAccessor(start + docId + 4),
            floatAccessor(start + docId + 5),
            floatAccessor(start + docId + 6),
            floatAccessor(start + docId + 7)
        };
        auto writePtr = (ui32*)(result + docId);
        for (const auto border : borders) {
            writePtr[0] += (val[0] > border) + ((val[1] > border) << 8) + ((val[2] > border) << 16) + ((val[3] > border) << 24);
            writePtr[1] += (val[4] > border) + ((val[5] > border) << 8) + ((val[6] > border) << 16) + ((val[7] > border) << 24);
        }
    }
    for (size_t docId = docCount8; docId < docCount; ++docId) {
        const auto val = floatAccessor(start + docId);
        for (const auto border : borders) {
            result[docId] += (ui8)(val > border);
        }
    }
    result += docCount;
}

/**
* This function binarizes
*/
template<typename TFloatFeatureAccessor, typename TCatFeatureAccessor>
inline void BinarizeFeatures(
    const TFullModel& model,
    TFloatFeatureAccessor floatAccessor,
    TCatFeatureAccessor catFeatureAccessor,
    size_t start,
    size_t end,
    TArrayRef<ui8> result,
    TVector<int>& transposedHash,
    TVector<float>& ctrs
) {
    const auto docCount = end - start;
    ui8* resultPtr = result.data();
    for (const auto& floatFeature : model.ObliviousTrees.FloatFeatures) {
        BinarizeFloats(docCount, [&floatFeature, floatAccessor](size_t index) { return floatAccessor(floatFeature, index); }, floatFeature.Borders, start, resultPtr);
    }
    auto catFeatureCount = model.ObliviousTrees.CatFeatures.size();
    if (catFeatureCount > 0) {
        for (size_t docId = 0; docId < docCount; ++docId) {
            auto idx = docId;
            for (size_t i = 0; i < catFeatureCount; ++i) {
                transposedHash[idx] = catFeatureAccessor(i, start + docId);
                idx += docCount;
            }
        }
        THashMap<int, int> catFeaturePackedIndexes;
        for (int i = 0; i < model.ObliviousTrees.CatFeatures.ysize(); ++i) {
            catFeaturePackedIndexes[model.ObliviousTrees.CatFeatures[i].FeatureIndex] = i;
        }
        OneHotBinsFromTransposedCatFeatures(model.ObliviousTrees.OneHotFeatures, catFeaturePackedIndexes, docCount, resultPtr, transposedHash);
        model.CtrProvider->CalcCtrs(
            model.ObliviousTrees.GetUsedModelCtrs(),
            result,
            transposedHash,
            docCount,
            ctrs
        );
        for (size_t i = 0; i < model.ObliviousTrees.CtrFeatures.size(); ++i) {
            const auto& ctr = model.ObliviousTrees.CtrFeatures[i];
            auto ctrFloatsPtr = &ctrs[i * docCount];
            BinarizeFloats(docCount, [ctrFloatsPtr](size_t index) { return ctrFloatsPtr[index]; }, ctr.Borders, 0, resultPtr);
        }
    }
}

using TCalcerIndexType = ui32;

using TTreeCalcFunction = std::function<void(const TFullModel& model,
    size_t blockStart,
    const ui8* __restrict binFeatures,
    size_t docCountInBlock,
    TCalcerIndexType* __restrict indexesVec,
    size_t treeStart,
    size_t treeEnd,
    double* __restrict results)>;

void CalcIndexes(
    bool needXorMask,
    const ui8* __restrict binFeatures,
    size_t docCountInBlock,
    ui32* __restrict indexesVec,
    const ui32* __restrict treeSplitsCurPtr,
    int curTreeSize);

TTreeCalcFunction GetCalcTreesFunction(int approxDimension, size_t docCountInBlock, bool hasOneHots);

template<typename TFloatFeatureAccessor, typename TCatFeatureAccessor>
inline void CalcGeneric(
    const TFullModel& model,
    TFloatFeatureAccessor floatFeatureAccessor,
    TCatFeatureAccessor catFeaturesAccessor,
    size_t docCount,
    size_t treeStart,
    size_t treeEnd,
    TArrayRef<double> results)
{
    size_t blockSize = FORMULA_EVALUATION_BLOCK_SIZE;
    blockSize = Min(blockSize, docCount);
    TVector<ui8> binFeatures(blockSize * model.ObliviousTrees.GetEffectiveBinaryFeaturesBucketsCount());
    auto calcTrees = GetCalcTreesFunction(model.ObliviousTrees.ApproxDimension, blockSize, !model.ObliviousTrees.OneHotFeatures.empty());
    if (docCount == 1) {
        CB_ENSURE((int)results.size() == model.ObliviousTrees.ApproxDimension);
        std::fill(results.begin(), results.end(), 0.0);
        TVector<int> transposedHash(model.ObliviousTrees.CatFeatures.size());
        TVector<float> ctrs(model.ObliviousTrees.GetUsedModelCtrs().size());
        BinarizeFeatures(
            model,
            floatFeatureAccessor,
            catFeaturesAccessor,
            0,
            1,
            binFeatures,
            transposedHash,
            ctrs
        );
        calcTrees(
                model,
                0,
                binFeatures.data(),
                1,
                nullptr,
                treeStart,
                treeEnd,
                results.data()
            );
        return;
    }

    CB_ENSURE(results.size() == docCount * model.ObliviousTrees.ApproxDimension);
    std::fill(results.begin(), results.end(), 0.0);
    TVector<TCalcerIndexType> indexesVec(blockSize);
    TVector<int> transposedHash(blockSize * model.ObliviousTrees.CatFeatures.size());
    TVector<float> ctrs(model.ObliviousTrees.GetUsedModelCtrs().size() * blockSize);
    for (size_t blockStart = 0; blockStart < docCount; blockStart += blockSize) {
        const auto docCountInBlock = Min(blockSize, docCount - blockStart);
        BinarizeFeatures(
            model,
            floatFeatureAccessor,
            catFeaturesAccessor,
            blockStart,
            blockStart + docCountInBlock,
            binFeatures,
            transposedHash,
            ctrs
        );
        calcTrees(
            model,
            blockStart,
            binFeatures.data(),
            docCountInBlock,
            indexesVec.data(),
            treeStart,
            treeEnd,
            results.data()
        );
    }
}


/**
 * Warning: use aggressive caching. Stores all binarized features in RAM
 */
class TFeatureCachedTreeEvaluator {
public:
    template<typename TFloatFeatureAccessor,
             typename TCatFeatureAccessor>
    TFeatureCachedTreeEvaluator(const TFullModel& model,
                                TFloatFeatureAccessor floatFeatureAccessor,
                                TCatFeatureAccessor catFeaturesAccessor,
                                size_t docCount)
            : Model(model)
            , DocCount(docCount) {
        size_t blockSize = FORMULA_EVALUATION_BLOCK_SIZE;
        BlockSize = Min(blockSize, docCount);
        CalcFunction = GetCalcTreesFunction(
                Model.ObliviousTrees.ApproxDimension,
                BlockSize,
                !Model.ObliviousTrees.OneHotFeatures.empty()
        );
        TVector<int> transposedHash(blockSize * model.ObliviousTrees.CatFeatures.size());
        TVector<float> ctrs(model.ObliviousTrees.GetUsedModelCtrs().size() * blockSize);
        {
            for (size_t blockStart = 0; blockStart < docCount; blockStart += blockSize) {
                const auto docCountInBlock = Min(blockSize, docCount - blockStart);
                TVector<ui8> binFeatures(model.ObliviousTrees.GetEffectiveBinaryFeaturesBucketsCount() * blockSize);
                BinarizeFeatures(
                        model,
                        floatFeatureAccessor,
                        catFeaturesAccessor,
                        blockStart,
                        blockStart + docCountInBlock,
                        binFeatures,
                        transposedHash,
                        ctrs
                );
                BinFeatures.push_back(std::move(binFeatures));
            }
        }
    }

    void Calc(size_t treeStart, size_t treeEnd, TArrayRef<double> results) const;
private:
    const TFullModel& Model;
    TVector<TVector<ui8>> BinFeatures;
    TTreeCalcFunction CalcFunction;
    ui64 DocCount;
    ui64 BlockSize;
};

template<typename TFloatFeatureAccessor, typename TCatFeatureAccessor>
inline TVector<TVector<double>> CalcTreeIntervalsGeneric(
    const TFullModel& model,
    TFloatFeatureAccessor floatFeatureAccessor,
    TCatFeatureAccessor catFeaturesAccessor,
    size_t docCount,
    size_t incrementStep)
{
    size_t blockSize = FORMULA_EVALUATION_BLOCK_SIZE;
    blockSize = Min(blockSize, docCount);
    auto treeStepCount = (model.ObliviousTrees.TreeSizes.size() + incrementStep - 1) / incrementStep;
    TVector<TVector<double>> results(docCount, TVector<double>(treeStepCount));
    CB_ENSURE(model.ObliviousTrees.ApproxDimension == 1);
    TVector<ui8> binFeatures(model.ObliviousTrees.GetEffectiveBinaryFeaturesBucketsCount() * blockSize);
    TVector<TCalcerIndexType> indexesVec(blockSize);
    TVector<int> transposedHash(blockSize * model.ObliviousTrees.CatFeatures.size());
    TVector<float> ctrs(model.ObliviousTrees.GetUsedModelCtrs().size() * blockSize);
    TVector<double> tmpResult(docCount);
    TArrayRef<double> tmpResultRef(tmpResult);
    auto calcTrees = GetCalcTreesFunction(model.ObliviousTrees.ApproxDimension, blockSize, !model.ObliviousTrees.OneHotFeatures.empty());
    for (size_t blockStart = 0; blockStart < docCount; blockStart += blockSize) {
        const auto docCountInBlock = Min(blockSize, docCount - blockStart);
        BinarizeFeatures(
            model,
            floatFeatureAccessor,
            catFeaturesAccessor,
            blockStart,
            blockStart + docCountInBlock,
            binFeatures,
            transposedHash,
            ctrs
        );
        for (size_t stepIdx = 0; stepIdx < treeStepCount; ++stepIdx) {
            calcTrees(
                model,
                blockStart,
                binFeatures.data(),
                docCountInBlock,
                indexesVec.data(),
                stepIdx * incrementStep,
                Min((stepIdx + 1) * incrementStep, model.ObliviousTrees.TreeSizes.size()),
                tmpResultRef.data()
            );
            for (size_t i = 0; i < docCountInBlock; ++i) {
                results[blockStart + i][stepIdx] = tmpResult[i];
            }
        }
    }
    return results;
}
