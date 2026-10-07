#ifndef FIXED_CNN_INT8_H
#define FIXED_CNN_INT8_H
#include <stddef.h>
#include <stdint.h>
#define CNN_INPUT_TICKS 235
#define CNN_PACKET_BYTES 598
#define CNN_T1 118
#define CNN_T2 59
#define CNN_MACS 33824
typedef struct { int16_t a1[8 * CNN_T1], a2[16 * CNN_T2]; } cnn_workspace_t;
void cnn_run(const uint8_t input[CNN_PACKET_BYTES], cnn_workspace_t *work,
             int32_t scores[6], uint32_t *saturations);
const char *cnn_coefficient_hash(void);
size_t cnn_workspace_bytes(void);
#endif
