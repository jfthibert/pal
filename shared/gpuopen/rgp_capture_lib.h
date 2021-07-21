#ifndef RGP_CAPTURE_LIB_H__
#define RGP_CAPTURE_LIB_H__

#ifdef __cplusplus
extern "C" {
#endif

// client_id can be set to -1 in order to auto detect
int CaptureRgp(char* hostname, char* filename, int preparationFrames, int clientId, uint64_t pipelineHash);

#ifdef __cplusplus
}
#endif

#endif  // RGP_CAPTURE_LIB_H__
