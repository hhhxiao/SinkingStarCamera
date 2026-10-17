#ifndef ORTHOCAM_CAMERA_H
#define ORTHOCAM_CAMERA_H

#include <cstddef>
#include <cstdint>

namespace camera {

    // Camera_Control component, recovered from the Jai Type_Info table in
    // sinking_star.exe (type name at rva 0x67B0A8, Type_Info at 0x67B090,
    // size 0x430). Only the fields this mod needs are named.
    //
    //   0x108  type_info pointer (points at exe base + 0x67B090)
    //   0x340  fov_vertical
    //   0x344  z_near
    //   0x348  z_far
    //   0x360  up_distance
    //   0x364  south_distance
    //   0x368  look_at_south_distance
    //   0x36C  room_pan_rate
    //   0x378  orthoness (read as a byte by the engine)
    //
    // The mod multiplies the three *distance* fields by the same factor, which is
    // what moves the overworld camera closer to or further from the map. The
    // engine re-reads these every frame, so the values are re-applied each tick.
    struct CameraControl {
        uint8_t _hdr[0x108];
        void *type_info;                // +0x108 == exe base + 0x67B090
        uint8_t _pad0[0x230];
        float fov_vertical;             // +0x340
        float z_near;                   // +0x344
        float z_far;                    // +0x348
        uint8_t _pad1[0x14];
        float up_distance;              // +0x360
        float south_distance;           // +0x364
        float look_at_south_distance;   // +0x368
        float room_pan_rate;            // +0x36C
        uint8_t _pad2[0x8];
        uint8_t orthoness;              // +0x378
        uint8_t _pad2b[3];
        float fov_expansion;            // +0x37C
        float fog_blur;                 // +0x380
        uint8_t _pad3[0xAC];
    };
    static_assert(sizeof(CameraControl) == 0x430, "Camera_Control layout drifted");

    void init();
    void tick();
    void shutdown();

    // Multiplies the current zoom factor. Values below 1.0 move the camera in.
    void zoom(float factor);
    void reset();
    void nudge_fov(float factor);
    float scale();
    float fov_scale();
    size_t entity_count();

}  // namespace camera

#endif
