#ifndef ACLOPRUNNER_HPP_
#define ACLOPRUNNER_HPP_

#include <acl/acl.h>
#include <acl/acl_op.h>
#include <acl/acl_op_compiler.h>
#include <diopi/diopirt.h>
#include <stdint.h>

#include <algorithm>
#include <array>
#include <functional>
#include <initializer_list>
#include <sstream>
#include <vector>

namespace impl {
namespace ascend {

#define CALL_ACLRT(Expr)                                                                \
    {                                                                                   \
        ::aclError ret = Expr;                                                          \
        if (ret != ::ACL_SUCCESS) {                                                     \
            printf("call a ascendrt function (%s) failed. return code=%d", #Expr, ret); \
        }                                                                               \
    }

#define warning(...)                             \
    printf("[%s:%d]: ", __FUNCTION__, __LINE__); \
    printf(__VA_ARGS__);                         \
    printf("\n");

#define info(...)                                \
    printf("[%s:%d]: ", __FUNCTION__, __LINE__); \
    printf(__VA_ARGS__);                         \
    printf("\n");

#define check_args(condition, ...)                   \
    if (!(condition)) {                              \
        printf("[%s:%d]: ", __FUNCTION__, __LINE__); \
        printf(__VA_ARGS__);                         \
        printf("\n");                                \
        std::abort();                                \
    }

inline aclDataType getAclDataType(diopiConstTensorHandle_t th) {
    diopiDtype_t type;
    diopiGetTensorDtype(th, &type);
    switch (type) {
        case diopi_dtype_float16:
            return ACL_FLOAT16;
        case diopi_dtype_float32:
            return ACL_FLOAT;
        case diopi_dtype_float64:
            return ACL_DOUBLE;
        case diopi_dtype_int8:
            return ACL_INT8;
        case diopi_dtype_uint8:
            return ACL_UINT8;
        case diopi_dtype_int16:
            return ACL_INT16;
        case diopi_dtype_uint16:
            return ACL_UINT16;
        case diopi_dtype_int32:
            return ACL_INT32;
        case diopi_dtype_uint32:
            return ACL_UINT32;
        case diopi_dtype_int64:
            return ACL_INT64;
        case diopi_dtype_uint64:
            return ACL_UINT64;
        case diopi_dtype_bool:
            return ACL_BOOL;
    }
    check_args(false, "acl not support dioptDtype_t:%d", type);
    return ACL_DT_UNDEFINED;
}

inline aclFormat getAclDataFormat(diopiConstTensorHandle_t th) {
    diopiSize_t shape;
    diopiSize_t stride;
    diopiGetTensorShape(th, &shape);
    diopiGetTensorStride(th, &stride);
    if (shape.len == 4) {
        return ACL_FORMAT_NCHW;
    }
    return ACL_FORMAT_ND;
}

inline bool is_integral_type(const diopiDtype_t& type) {
    switch (type) {
        case diopi_dtype_bool:
        case diopi_dtype_int8:
        case diopi_dtype_uint8:
        case diopi_dtype_int16:
        case diopi_dtype_uint16:
        case diopi_dtype_int32:
        case diopi_dtype_uint32:
        case diopi_dtype_int64:
        case diopi_dtype_uint64:
            return true;
    }
    return false;
}

template <typename T>
T getValue(const diopiScalar_t* scalar) {
    check_args(scalar != nullptr, "input should not be nullptr");
    if (is_integral_type(scalar->stype)) {
        return static_cast<T>(scalar->ival);
    } else {
        return static_cast<T>(scalar->fval);
    }
}

template <int InputSize = 8, int OutputSize = 8, aclDataType (*dtypeCastStrategy)(diopiConstTensorHandle_t) = getAclDataType>
class AclOpRunner {
    std::string opname_;
    aclopAttr* attr_;
    std::array<aclTensorDesc*, InputSize> inputDescs_;
    std::array<aclDataBuffer*, InputSize> inputBuffers_;
    std::array<aclTensorDesc*, OutputSize> outputDescs_;
    std::array<aclDataBuffer*, OutputSize> outputBuffers_;

    std::string dumpRunnerInfo() {
        std::stringstream sstream;
        sstream << "opname:" << opname_ << ",ins.size:" << InputSize << ",outs.size:" << OutputSize << std::endl;
        return sstream.str();
    }

public:
    AclOpRunner(std::string opname) : opname_(std::move(opname)), attr_(aclopCreateAttr()) {
        inputDescs_.fill(nullptr);
        inputBuffers_.fill(nullptr);
        outputDescs_.fill(nullptr);
        inputBuffers_.fill(nullptr);
    }

    ~AclOpRunner() {
        aclopDestroyAttr(attr_);
        auto destoryAclTensorDesc = [](aclTensorDesc* desc) {
            if (desc) {
                aclDestroyTensorDesc(desc);
            }
        };
        auto destoryAclDataBuffer = [](aclDataBuffer* buffer) {
            if (buffer) {
                aclDestroyDataBuffer(buffer);
            }
        };
        std::for_each(inputDescs_.begin(), inputDescs_.end(), destoryAclTensorDesc);
        std::for_each(outputDescs_.begin(), outputDescs_.end(), destoryAclTensorDesc);
        std::for_each(inputBuffers_.begin(), inputBuffers_.end(), destoryAclDataBuffer);
        std::for_each(outputBuffers_.begin(), outputBuffers_.end(), destoryAclDataBuffer);
    }

    AclOpRunner& addInput(const int index, diopiConstTensorHandle_t th, const aclFormat& format) {
        check_args(th != nullptr, "input should not be nullptr");
        diopiSize_t shape;
        diopiSize_t stride;
        int64_t numel = 0;
        int64_t itemsize = 0;
        const void* ptr = nullptr;
        diopiGetTensorShape(th, &shape);
        diopiGetTensorStride(th, &stride);
        diopiGetTensorNumel(th, &numel);
        diopiGetTensorElemSize(th, &itemsize);
        diopiGetTensorDataConst(th, &ptr);

        std::vector<int64_t> dims(shape.len);
        for (size_t i = 0; i < dims.size(); ++i) {
            dims[i] = shape.data[i];
        }
        if (dims.size() == 0 && numel == 1) {
            dims.push_back(1);
        }

        int finalIndex = index;
        if (index < 0) {
            for (size_t i = 0; i < InputSize; i++) {
                if (inputDescs_[i] == nullptr) {
                    finalIndex = i;
                    break;
                }
            }
        }

        check_args(finalIndex >= 0 && finalIndex < InputSize, "check 0<=finalIndex<InputSize failed");

        auto& desc = inputDescs_[finalIndex];
        auto& buffer = inputBuffers_[finalIndex];

        desc = aclCreateTensorDesc(dtypeCastStrategy(th), dims.size(), dims.data(), format);
        check_args(desc != nullptr, "aclTensorDesc should not be nullptr.");
        buffer = aclCreateDataBuffer(const_cast<void*>(ptr), numel * itemsize);
        return *this;
    }

    template <int index = -1>
    AclOpRunner& addInput(diopiConstTensorHandle_t th, const aclFormat& format) {
        static_assert(index < InputSize);
        return addInput(index, th, format);
    }

    AclOpRunner& addInput(const int index, diopiConstTensorHandle_t th) { return addInput(index, th, getAclDataFormat(th)); }

    template <int index = -1>
    AclOpRunner& addInput(diopiConstTensorHandle_t th) {
        static_assert(index < InputSize);
        return addInput(index, th, getAclDataFormat(th));
    }

    template <typename... Ins>
    AclOpRunner& addInput(diopiConstTensorHandle_t in, const Ins&... ins) {
        addInput<-1>(in).addInput(ins...);
        return *this;
    }

    AclOpRunner& addOutput(const int index, diopiTensorHandle_t th, const aclFormat format) {
        check_args(th != nullptr, "output should not be nullptr");
        diopiSize_t shape;
        diopiSize_t stride;
        int64_t numel = 0;
        int64_t itemsize = 0;
        void* ptr = nullptr;
        diopiGetTensorShape(th, &shape);
        diopiGetTensorStride(th, &stride);
        diopiGetTensorNumel(th, &numel);
        diopiGetTensorElemSize(th, &itemsize);
        diopiGetTensorData(th, &ptr);

        std::vector<int64_t> dims(shape.len);
        for (size_t i = 0; i < dims.size(); ++i) {
            dims[i] = shape.data[i];
        }
        if (dims.size() == 0 && numel == 1) {
            dims.push_back(1);
        }

        int finalIndex = index;
        if (index < 0) {
            for (size_t i = 0; i < OutputSize; i++) {
                if (outputDescs_[i] == nullptr) {
                    finalIndex = i;
                    break;
                }
            }
        }
        check_args(finalIndex >= 0 && finalIndex < OutputSize, "check 0<=finalIndex<OutputSize failed");

        auto& desc = outputDescs_[finalIndex];
        auto& buffer = outputBuffers_[finalIndex];
        desc = aclCreateTensorDesc(dtypeCastStrategy(th), dims.size(), dims.data(), format);
        check_args(desc != nullptr, "aclTensorDesc should not be nullptr.");
        buffer = aclCreateDataBuffer(ptr, numel * itemsize);
        return *this;
    }

    template <int index = -1>
    AclOpRunner& addOutput(diopiTensorHandle_t th) {
        return addOutput(index, th, getAclDataFormat(th));
    }

    template <int index = -1>
    AclOpRunner& addOutput(diopiTensorHandle_t th, const aclFormat format) {
        return addOutput(index, th, format);
    }

    template <typename... Outs>
    AclOpRunner& addOutput(diopiTensorHandle_t out, Outs&... outs) {
        return addOutput<-1>(out).addOutput(outs...);
    }

    template <typename T>
    AclOpRunner& setAttr(const std::string& attrName, const T& value) {
        if constexpr (std::is_same<T, int64_t>::value || std::is_same<T, int>::value) {
            CALL_ACLRT(aclopSetAttrInt(attr_, attrName.data(), value));
            return *this;
        }
        if constexpr (std::is_same<T, float>::value) {
            CALL_ACLRT(aclopSetAttrFloat(attr_, attrName.data(), value));
            return *this;
        }
        if constexpr (std::is_same<T, uint8_t>::value || std::is_same<T, bool>::value) {
            CALL_ACLRT(aclopSetAttrBool(attr_, attrName.data(), value));
            return *this;
        }
        if constexpr (std::is_same<T, std::string>::value) {
            CALL_ACLRT(aclopSetAttrString(attr_, attrName.data(), value.data()));
            return *this;
        }
        check_args(false, "no specialization for this type.");
        return *this;
    }

    template <typename T>
    AclOpRunner& setAttr(const std::string& attrName, const typename std::vector<T>& value) {
        std::vector<int64_t> vec(value.begin(), value.end());
        CALL_ACLRT(aclopSetAttrListInt(attr_, attrName.data(), vec.size(), vec.data()));
        return *this;
    }

    template <aclEngineType EngineType = ACL_ENGINE_SYS, aclCompileType CompileType = ACL_COMPILE_SYS>
    AclOpRunner& run(diopiContextHandle_t& ctx) {
        diopiStreamHandle_t stream;
        diopiGetStream(ctx, &stream);
        int inSize = 0;
        for (size_t i = 0; i < InputSize; i++) {
            if (inputDescs_[i] != nullptr) {
                inSize++;
            }
        }
        int outSize = 0;
        for (size_t i = 0; i < OutputSize; i++) {
            if (outputDescs_[i] != nullptr) {
                outSize++;
            }
        }

        auto errorcode = aclopCompileAndExecute(opname_.data(),
                                                inSize,
                                                inputDescs_.data(),
                                                inputBuffers_.data(),
                                                outSize,
                                                outputDescs_.data(),
                                                outputBuffers_.data(),
                                                attr_,
                                                EngineType,
                                                CompileType,
                                                nullptr,
                                                stream);
        if (errorcode != ACL_SUCCESS) {
            warning((dumpRunnerInfo() + ":" + aclGetRecentErrMsg()).c_str());
        }
        // check_args(errorcode == ACL_SUCCESS, dumpRunnerInfo().c_str());
        //   Get environment variables once when run is called for the first time
        static int PARROTS_DEBUG_ACLOPRUNNER = std::getenv("DIOPI_DEBUG_ACLOPRUNNER") == nullptr ? 0 : 1;
        if (PARROTS_DEBUG_ACLOPRUNNER > 0) {
            info(dumpRunnerInfo().c_str());
        }

        return *this;
    }
};

}  // namespace ascend
}  // namespace impl

#endif  //  ACLOPRUNNER_HPP_