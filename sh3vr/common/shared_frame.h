#pragma once

#include <cstdint>

// Shared-memory ABI between the 32-bit game proxy and the 64-bit OpenXR host.
// Keep this header free of pointer-sized fields so both processes use the same
// binary layout.

inline constexpr wchar_t SH3VR_SECTION_NAME[] = L"Local\\sh3vr_frame_v11";
inline constexpr wchar_t SH3VR_EVENT_NAME[] = L"Local\\sh3vr_frame_ready_v11";

inline constexpr std::uint32_t SH3VR_MAGIC = 0x56334853u; // "SH3V"
inline constexpr std::uint32_t SH3VR_VERSION = 11;
inline constexpr std::uint32_t SH3VR_HEADER_BYTES = 1024;
inline constexpr std::uint32_t SH3VR_SLOT_COUNT = 2;
inline constexpr std::uint32_t SH3VR_BYTES_PER_PX = 4;
inline constexpr std::uint32_t SH3VR_MAX_WIDTH = 2560;
inline constexpr std::uint32_t SH3VR_MAX_HEIGHT = 1440;
inline constexpr std::uint32_t SH3VR_SLOT_BYTES =
    SH3VR_MAX_WIDTH * SH3VR_MAX_HEIGHT * SH3VR_BYTES_PER_PX;
inline constexpr std::uint32_t SH3VR_SECTION_BYTES =
    SH3VR_HEADER_BYTES + SH3VR_SLOT_COUNT * SH3VR_SLOT_BYTES;

enum Sh3VrPixelLayout : std::uint32_t
{
    SH3VR_PIXEL_UNKNOWN = 0,
    SH3VR_PIXEL_BGRA8 = 1
};

enum Sh3VrPoseFlags : std::uint32_t
{
    SH3VR_POSE_NONE = 0,
    SH3VR_POSE_ORIENTATION_VALID = 1u << 0,
    SH3VR_POSE_POSITION_VALID = 1u << 1,
    SH3VR_POSE_ORIENTATION_TRACKED = 1u << 2,
    SH3VR_POSE_POSITION_TRACKED = 1u << 3
};

enum Sh3VrRenderMode : std::uint32_t
{
    SH3VR_RENDER_CINEMA = 0,
    SH3VR_RENDER_IMMERSIVE_MONO = 1,
    SH3VR_RENDER_IMMERSIVE_STEREO = 2
};

enum Sh3VrRenderFlags : std::uint32_t
{
    SH3VR_RENDER_FLAG_NONE = 0,
    // The native eye textures contain a bounded geometry replay. The Host
    // must composite them over the complete shared game backbuffer.
    SH3VR_RENDER_FLAG_PARTIAL_NATIVE_EYES = 1u << 0,
    // The game submitted its high-volume screen-space fog pass this frame.
    // Native eyes omit that pass and let the Host provide a cheap world-
    // anchored replacement without replaying hundreds of D3D8 billboards.
    SH3VR_RENDER_FLAG_PROCEDURAL_FOG = 1u << 1
};

enum Sh3VrControllerButton : std::uint32_t
{
    SH3VR_BUTTON_NONE = 0,
    SH3VR_BUTTON_A = 1u << 0,
    SH3VR_BUTTON_B = 1u << 1,
    SH3VR_BUTTON_X = 1u << 2,
    SH3VR_BUTTON_Y = 1u << 3,
    SH3VR_BUTTON_LEFT_TRIGGER = 1u << 4,
    SH3VR_BUTTON_RIGHT_TRIGGER = 1u << 5,
    SH3VR_BUTTON_LEFT_GRIP = 1u << 6,
    SH3VR_BUTTON_RIGHT_GRIP = 1u << 7,
    SH3VR_BUTTON_LEFT_STICK = 1u << 8,
    SH3VR_BUTTON_RIGHT_STICK = 1u << 9,
    SH3VR_BUTTON_MENU = 1u << 10
};

enum Sh3VrRoomscaleMovement : std::uint32_t
{
    SH3VR_ROOMSCALE_NONE = 0,
    SH3VR_ROOMSCALE_FORWARD = 1u << 0,
    SH3VR_ROOMSCALE_BACKWARD = 1u << 1,
    SH3VR_ROOMSCALE_LEFT = 1u << 2,
    SH3VR_ROOMSCALE_RIGHT = 1u << 3
};

struct Sh3VrHeadPose
{
    std::uint32_t flags;
    std::uint32_t reserved;
    std::int64_t predictedDisplayTime;
    float position[3];
    float orientation[4];
};

static_assert(sizeof(Sh3VrHeadPose) == 48,
    "The shared head pose ABI must remain exactly 48 bytes.");

struct Sh3VrControllerPose
{
    std::uint32_t flags;
    std::uint32_t reserved;
    float position[3];
    float orientation[4];
};

static_assert(sizeof(Sh3VrControllerPose) == 36,
    "The shared controller pose ABI must remain exactly 36 bytes.");

struct Sh3VrControllerState
{
    std::uint32_t active;
    std::uint32_t reserved0;
    std::int64_t predictedDisplayTime;
    float thumbstick[2][2];
    float trigger[2];
    float squeeze[2];
    std::uint32_t buttons;
    std::uint32_t touches;
    Sh3VrControllerPose gripPose[2];
    Sh3VrControllerPose aimPose[2];
    std::uint32_t reserved[6];
};

static_assert(sizeof(Sh3VrControllerState) == 224,
    "The shared controller state ABI must remain exactly 224 bytes.");

// Optional diagnostic payload stored at the beginning of
// Sh3VrFrameHeader::reserved. It does not change the shared-memory ABI.
inline constexpr std::uint32_t SH3VR_WEAPON_DEBUG_MAGIC = 0x47554244u;
struct Sh3VrWeaponDebugState
{
    std::uint32_t magic;
    std::uint32_t weaponValid;
    std::int32_t profileIndex;
    float pitchDegrees;
    float yawDegrees;
    float rollDegrees;
    std::uint32_t leftHandValid;
    float leftHandPitchDegrees;
    float leftHandYawDegrees;
    float leftHandRollDegrees;
};

static_assert(sizeof(Sh3VrWeaponDebugState) == 40,
    "Weapon debug state must fit in the reserved frame-header area.");

// Host-owned projection metadata stored separately from the proxy-owned
// weapon diagnostics in Sh3VrFrameHeader::reserved.  The native eye textures
// are sampled through asymmetric OpenXR FOV rectangles.  Screen-space UI must
// know those rectangles so its two copies land on the same final display ray.
inline constexpr std::uint32_t SH3VR_PROJECTION_UV_MAGIC = 0x56505553u;
inline constexpr std::uint32_t SH3VR_PROJECTION_UV_RESERVED_OFFSET = 64u;
struct Sh3VrProjectionUvState
{
    std::uint32_t magic;
    volatile std::int32_t sequence;
    float eyeRect[2][4];
};

static_assert(sizeof(Sh3VrProjectionUvState) == 40,
    "Projection UV state must fit in the reserved frame-header area.");

struct Sh3VrFrameHeader
{
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t headerBytes;
    std::uint32_t slotCount;
    std::uint32_t slotBytes;
    std::uint32_t pixelLayout;
    std::uint32_t producerPid;
    volatile std::int32_t producerAlive;
    volatile std::int32_t publishedFrame;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t pitch;
    std::uint32_t contentLeft;
    std::uint32_t contentTop;
    std::uint32_t contentWidth;
    std::uint32_t contentHeight;
    std::int64_t qpcFrequency;
    std::int64_t qpcCapture;
    volatile std::int32_t headPoseSequence;
    std::uint32_t headPoseReserved;
    Sh3VrHeadPose headPose;
    volatile std::int32_t renderMode;
    volatile std::uint32_t renderFlags;
    volatile std::int32_t renderEye;
    std::uint32_t renderEyeReserved;
    volatile std::int32_t stereoPairSequence;
    volatile std::int32_t stereoReadyMask;
    Sh3VrHeadPose frameRenderPose;
    // Handle values are meaningful only in producerPid. The Host duplicates
    // them into its own process before using them as GPU frame sources.
    std::uint64_t d3d12BackBufferHandles[2];
    std::uint32_t d3d12BackBufferGeneration;
    std::uint32_t d3d12BackBufferIndex;
    std::uint32_t d3d12BackBufferCount;
    std::uint32_t d3d12BackBufferReserved;
    volatile std::uint32_t requestedEyeWidth;
    volatile std::uint32_t requestedEyeHeight;
    volatile std::uint32_t requestedEyeSampleCount;
    volatile std::uint32_t requestedEyeGeneration;
    // Native per-eye color targets created by the D3D8-on-12 backend.
    // Handle values are meaningful only in producerPid.
    // Two complete eye pairs allow the producer to update one pair while the
    // Host displays the other. Layout is set * 2 + eye.
    std::uint64_t d3d12EyeTextureHandles[4];
    std::uint32_t d3d12EyeTextureGeneration;
    std::uint32_t d3d12EyeTextureWidth;
    std::uint32_t d3d12EyeTextureHeight;
    std::uint32_t d3d12EyeTextureFormat;
    volatile std::uint32_t d3d12EyeTextureActiveSet;
    volatile std::uint32_t d3d12EyeTextureFrameSequence;
    Sh3VrHeadPose d3d12EyeFrameRenderPoses[2];
    // SH3 applies its location-dependent RGB correction as a fixed-function
    // multiply/blend pass over the flat scene. The proxy publishes only the
    // numeric parameters so the Host can apply the same operation separately
    // to the already-rendered left and right eye textures.
    volatile std::uint32_t gamePostProcessEnabled;
    float gamePostProcessScale[3];
    float gamePostProcessBlend;
    std::uint32_t gamePostProcessSource[4];
    std::uint32_t gamePostProcessIntensity;
    volatile std::int32_t controllerStateSequence;
    std::uint32_t controllerStateReserved;
    Sh3VrControllerState controllerState;
    std::uint8_t reserved[SH3VR_HEADER_BYTES - 680];
};

static_assert(sizeof(Sh3VrFrameHeader) == SH3VR_HEADER_BYTES,
    "The shared frame header ABI must remain exactly 1024 bytes.");
