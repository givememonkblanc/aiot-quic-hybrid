#include "tinyvlm.h"
#include <cmath>
#include <cstring>
#include <new>
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"
namespace {
tflite::MicroMutableOpResolver<9> resolver;
tflite::MicroInterpreter *interpreter = nullptr;
bool registered = false;
}
extern "C" void tinyvlm_encoder_reset(void) { delete interpreter; interpreter=nullptr; }
extern "C" size_t tinyvlm_encoder_arena_used_bytes(void) {
    return interpreter ? interpreter->arena_used_bytes() : 0;
}
extern "C" bool tinyvlm_encoder_init(const uint8_t *bytes, size_t size, uint8_t *arena, size_t capacity)
{
    tinyvlm_encoder_reset();
    if (!bytes || size < 8 || !arena || !capacity || (reinterpret_cast<uintptr_t>(arena) & 15)) return false;
    flatbuffers::Verifier verifier(bytes, size);
    if (!tflite::VerifyModelBuffer(verifier)) return false;
    const auto *model=tflite::GetModel(bytes);
    if (model->version()!=TFLITE_SCHEMA_VERSION) return false;
    if (!registered) {
        if (resolver.AddConv2D()!=kTfLiteOk || resolver.AddDepthwiseConv2D()!=kTfLiteOk ||
            resolver.AddAdd()!=kTfLiteOk || resolver.AddMean()!=kTfLiteOk ||
            resolver.AddFullyConnected()!=kTfLiteOk || resolver.AddReshape()!=kTfLiteOk ||
            resolver.AddPad()!=kTfLiteOk || resolver.AddRelu6()!=kTfLiteOk ||
            resolver.AddMul()!=kTfLiteOk) return false;
        registered=true;
    }
    interpreter=new(std::nothrow) tflite::MicroInterpreter(model,resolver,arena,capacity);
    if (!interpreter || interpreter->AllocateTensors()!=kTfLiteOk) { tinyvlm_encoder_reset(); return false; }
    auto *in=interpreter->input(0); auto *out=interpreter->output(0);
    bool valid=in && out && in->type==kTfLiteInt8 && out->type==kTfLiteInt8 &&
        in->dims->size==4 && in->dims->data[0]==1 && in->dims->data[1]==128 &&
        in->dims->data[2]==128 && in->dims->data[3]==3 && in->params.scale>0 &&
        out->dims->size==2 && out->dims->data[0]==1 && out->dims->data[1]<=256 &&
        out->dims->data[1]>=16 && out->params.scale>0;
    if (!valid) tinyvlm_encoder_reset();
    return valid;
}
extern "C" bool tinyvlm_encoder_run(const uint8_t *rgb,size_t size,int8_t *embedding,
    size_t capacity,size_t *dimensions,int *zero_point)
{
    if (!interpreter || !rgb || size!=128*128*3 || !embedding || !dimensions || !zero_point) return false;
    auto *in=interpreter->input(0);auto *out=interpreter->output(0);
    size_t dim=static_cast<size_t>(out->dims->data[1]);
    if (capacity<dim) return false;
    if (!tinyvlm_quantize_rgb(rgb,size,in->params.scale,in->params.zero_point,in->data.int8)) return false;
    if (interpreter->Invoke()!=kTfLiteOk) return false;
    std::memcpy(embedding,out->data.int8,dim);*dimensions=dim;*zero_point=out->params.zero_point;
    return true;
}
