#include <wayfire/plugin.hpp>
#include <wayfire/per-output-plugin.hpp>
#include <wayfire/output.hpp>
#include <wayfire/option-wrapper.hpp>
#include <wayfire/core.hpp>

extern "C"
{
#include <wayland-server-core.h>

#include <wlr/backend.h>
#include <wlr/backend/drm.h>
#include <wlr/types/wlr_output.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_mode.h>
}

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>


namespace
{

constexpr double RED_WEIGHT   = 0.2126;
constexpr double GREEN_WEIGHT = 0.7152;
constexpr double BLUE_WEIGHT  = 0.0722;

constexpr double SATURATION_MIN = 0.0;
constexpr double SATURATION_MAX = 3.0;

constexpr double BRIGHTNESS_MIN = 0.0;
constexpr double BRIGHTNESS_MAX = 2.0;
constexpr double BRIGHTNESS_DEFAULT = 1.0;

constexpr int MAX_RETRY_COUNT = 20;
constexpr int RETRY_DELAY_MS = 50;

constexpr uint32_t MAX_COLOROP_CHAIN = 64;


/*
 * DRM S31.32.
 *
 * Sign bit:
 *     bit 63
 *
 * Magnitude:
 *     bits 62..0
 */
static uint64_t double_to_drm_s31_32(double value)
{
    if (!std::isfinite(value))
        value = 0.0;

    constexpr double MAX_VALUE =
        static_cast<double>(0x7fffffff);

    value = std::clamp(
        value,
        -MAX_VALUE,
        MAX_VALUE);

    bool negative = value < 0.0;

    if (negative)
        value = -value;

    const long double scaled =
        static_cast<long double>(value) *
        static_cast<long double>(uint64_t{1} << 32);

    uint64_t magnitude;

    if (scaled >= static_cast<long double>(
            0x7fffffffffffffffULL))
    {
        magnitude =
            0x7fffffffffffffffULL;
    }
    else
    {
        magnitude =
            static_cast<uint64_t>(
                std::llround(scaled));
    }

    if (!negative)
        return magnitude;

    return
        (uint64_t{1} << 63) |
        magnitude;
}


/*
 * Classic DRM CRTC CTM.
 *
 * output = brightness * saturation_matrix(input)
 */
static drm_color_ctm make_saturation_ctm(
    double saturation,
    double brightness)
{
    saturation =
        std::clamp(
            saturation,
            SATURATION_MIN,
            SATURATION_MAX);

    brightness =
        std::clamp(
            brightness,
            BRIGHTNESS_MIN,
            BRIGHTNESS_MAX);

    const double inverse =
        1.0 - saturation;

    drm_color_ctm ctm{};

    ctm.matrix[0] =
        double_to_drm_s31_32(
            brightness *
            (inverse * RED_WEIGHT + saturation));

    ctm.matrix[1] =
        double_to_drm_s31_32(
            brightness *
            (inverse * GREEN_WEIGHT));

    ctm.matrix[2] =
        double_to_drm_s31_32(
            brightness *
            (inverse * BLUE_WEIGHT));


    ctm.matrix[3] =
        double_to_drm_s31_32(
            brightness *
            (inverse * RED_WEIGHT));

    ctm.matrix[4] =
        double_to_drm_s31_32(
            brightness *
            (inverse * GREEN_WEIGHT + saturation));

    ctm.matrix[5] =
        double_to_drm_s31_32(
            brightness *
            (inverse * BLUE_WEIGHT));


    ctm.matrix[6] =
        double_to_drm_s31_32(
            brightness *
            (inverse * RED_WEIGHT));

    ctm.matrix[7] =
        double_to_drm_s31_32(
            brightness *
            (inverse * GREEN_WEIGHT));

    ctm.matrix[8] =
        double_to_drm_s31_32(
            brightness *
            (inverse * BLUE_WEIGHT + saturation));

    return ctm;
}


/*
 * DRM 3x4 CTM.
 *
 * Brightness is intentionally NOT included here.
 * On AMD's plane color pipeline brightness is implemented
 * by the preceding MULTIPLIER colorop.
 */
static drm_color_ctm_3x4 make_saturation_ctm_3x4(
    double saturation)
{
    saturation =
        std::clamp(
            saturation,
            SATURATION_MIN,
            SATURATION_MAX);

    const double inverse =
        1.0 - saturation;

    drm_color_ctm_3x4 ctm{};

    ctm.matrix[0] =
        double_to_drm_s31_32(
            inverse * RED_WEIGHT + saturation);

    ctm.matrix[1] =
        double_to_drm_s31_32(
            inverse * GREEN_WEIGHT);

    ctm.matrix[2] =
        double_to_drm_s31_32(
            inverse * BLUE_WEIGHT);

    ctm.matrix[3] =
        double_to_drm_s31_32(0.0);


    ctm.matrix[4] =
        double_to_drm_s31_32(
            inverse * RED_WEIGHT);

    ctm.matrix[5] =
        double_to_drm_s31_32(
            inverse * GREEN_WEIGHT + saturation);

    ctm.matrix[6] =
        double_to_drm_s31_32(
            inverse * BLUE_WEIGHT);

    ctm.matrix[7] =
        double_to_drm_s31_32(0.0);


    ctm.matrix[8] =
        double_to_drm_s31_32(
            inverse * RED_WEIGHT);

    ctm.matrix[9] =
        double_to_drm_s31_32(
            inverse * GREEN_WEIGHT);

    ctm.matrix[10] =
        double_to_drm_s31_32(
            inverse * BLUE_WEIGHT + saturation);

    ctm.matrix[11] =
        double_to_drm_s31_32(0.0);

    return ctm;
}


/*
 * Find a DRM property.
 */
static uint32_t find_object_property(
    int fd,
    uint32_t object_id,
    uint32_t object_type,
    const char *name,
    uint64_t *current_value = nullptr)
{
    if (current_value)
        *current_value = 0;

    drmModeObjectProperties *properties =
        drmModeObjectGetProperties(
            fd,
            object_id,
            object_type);

    if (!properties)
        return 0;

    uint32_t result = 0;

    for (uint32_t i = 0;
         i < properties->count_props;
         ++i)
    {
        drmModePropertyRes *property =
            drmModeGetProperty(
                fd,
                properties->props[i]);

        if (!property)
            continue;

        if (std::strcmp(
                property->name,
                name) == 0)
        {
            result =
                property->prop_id;

            if (current_value)
            {
                *current_value =
                    properties->prop_values[i];
            }

            drmModeFreeProperty(property);
            break;
        }

        drmModeFreeProperty(property);
    }

    drmModeFreeObjectProperties(properties);

    return result;
}


/*
 * Read DRM property.
 */
static bool read_object_property(
    int fd,
    uint32_t object_id,
    uint32_t object_type,
    const char *name,
    uint64_t& value)
{
    value = 0;

    return find_object_property(
        fd,
        object_id,
        object_type,
        name,
        &value) != 0;
}


/*
 * Read enum/value list from a DRM property.
 */
static std::vector<uint64_t>
get_property_values(
    int fd,
    uint32_t property_id)
{
    std::vector<uint64_t> values;

    drmModePropertyRes *property =
        drmModeGetProperty(
            fd,
            property_id);

    if (!property)
        return values;

    if (property->flags & DRM_MODE_PROP_ENUM)
    {
        for (int i = 0;
             i < property->count_enums;
             ++i)
        {
            values.push_back(
                property->enums[i].value);
        }
    }
    else
    {
        for (int i = 0;
             i < property->count_values;
             ++i)
        {
            values.push_back(
                property->values[i]);
        }
    }

    drmModeFreeProperty(property);

    return values;
}


/*
 * Find the currently active CRTC for a connector.
 */
static uint32_t find_crtc_for_connector(
    int fd,
    uint32_t connector_id)
{
    drmModeConnector *connector =
        drmModeGetConnector(
            fd,
            connector_id);

    if (!connector)
        return 0;

    if (connector->encoder_id)
    {
        drmModeEncoder *encoder =
            drmModeGetEncoder(
                fd,
                connector->encoder_id);

        if (encoder)
        {
            uint32_t crtc =
                encoder->crtc_id;

            drmModeFreeEncoder(encoder);
            drmModeFreeConnector(connector);

            if (crtc)
                return crtc;
        }
    }

    for (int i = 0;
         i < connector->count_encoders;
         ++i)
    {
        drmModeEncoder *encoder =
            drmModeGetEncoder(
                fd,
                connector->encoders[i]);

        if (!encoder)
            continue;

        uint32_t crtc =
            encoder->crtc_id;

        drmModeFreeEncoder(encoder);

        if (crtc)
        {
            drmModeFreeConnector(connector);
            return crtc;
        }
    }

    drmModeRes *resources =
        drmModeGetResources(fd);

    if (!resources)
    {
        drmModeFreeConnector(connector);
        return 0;
    }

    for (int e = 0;
         e < connector->count_encoders;
         ++e)
    {
        drmModeEncoder *encoder =
            drmModeGetEncoder(
                fd,
                connector->encoders[e]);

        if (!encoder)
            continue;

        uint32_t possible_crtcs =
            encoder->possible_crtcs;

        drmModeFreeEncoder(encoder);

        for (int i = 0;
             i < resources->count_crtcs;
             ++i)
        {
            if (!(possible_crtcs & (1u << i)))
                continue;

            uint32_t crtc_id =
                resources->crtcs[i];

            drmModeCrtc *crtc =
                drmModeGetCrtc(
                    fd,
                    crtc_id);

            if (!crtc)
                continue;

            bool active =
                crtc->mode_valid &&
                crtc->buffer_id != 0;

            drmModeFreeCrtc(crtc);

            if (active)
            {
                drmModeFreeResources(resources);
                drmModeFreeConnector(connector);
                return crtc_id;
            }
        }
    }

    drmModeFreeResources(resources);
    drmModeFreeConnector(connector);

    return 0;
}


/*
 * Read a CRTC CTM blob.
 */
static std::optional<drm_color_ctm>
read_ctm_blob(
    int fd,
    uint32_t blob_id)
{
    if (blob_id == 0)
        return std::nullopt;

    drmModePropertyBlobRes *blob =
        drmModeGetPropertyBlob(
            fd,
            blob_id);

    if (!blob)
        return std::nullopt;

    std::optional<drm_color_ctm> result;

    if (blob->length >= sizeof(drm_color_ctm))
    {
        drm_color_ctm ctm{};

        std::memcpy(
            &ctm,
            blob->data,
            sizeof(ctm));

        result = ctm;
    }

    drmModeFreePropertyBlob(blob);

    return result;
}


/*
 * Find the active primary plane.
 */
static uint32_t find_primary_plane(
    int fd,
    uint32_t crtc_id)
{
    drmModePlaneRes *planes =
        drmModeGetPlaneResources(fd);

    if (!planes)
        return 0;

    uint32_t result = 0;

    for (uint32_t i = 0;
         i < planes->count_planes;
         ++i)
    {
        drmModePlane *plane =
            drmModeGetPlane(
                fd,
                planes->planes[i]);

        if (!plane)
            continue;

        if (plane->crtc_id != crtc_id)
        {
            drmModeFreePlane(plane);
            continue;
        }

        uint64_t type = 0;

        bool have_type =
            read_object_property(
                fd,
                plane->plane_id,
                DRM_MODE_OBJECT_PLANE,
                "type",
                type);

        if (have_type &&
            type == DRM_PLANE_TYPE_PRIMARY)
        {
            result =
                plane->plane_id;

            drmModeFreePlane(plane);
            break;
        }

        drmModeFreePlane(plane);
    }

    drmModeFreePlaneResources(planes);

    return result;
}


/*
 * One colorop discovered in a pipeline.
 */
struct colorop_info
{
    uint32_t id = 0;
    uint32_t type = UINT32_MAX;

    uint32_t bypass_property = 0;

    uint32_t data_property = 0;
    uint32_t multiplier_property = 0;
};


/*
 * Complete discovered color pipeline.
 */
struct color_pipeline_info
{
    bool valid = false;

    uint32_t plane_id = 0;

    uint32_t color_pipeline_property = 0;

    uint64_t original_pipeline = 0;
    uint64_t selected_pipeline = 0;

    std::vector<colorop_info> colorops;

    uint32_t multiplier_colorop = 0;
    uint32_t multiplier_property = 0;

    uint32_t ctm_colorop = 0;
    uint32_t ctm_data_property = 0;
};


/*
 * Get colorop TYPE.
 */
static uint32_t get_colorop_type(
    int fd,
    uint32_t colorop_id)
{
    uint64_t value = 0;

    if (!read_object_property(
            fd,
            colorop_id,
            DRM_MODE_OBJECT_COLOROP,
            "TYPE",
            value))
    {
        return UINT32_MAX;
    }

    return static_cast<uint32_t>(value);
}


/*
 * Get colorop NEXT.
 */
static uint32_t get_colorop_next(
    int fd,
    uint32_t colorop_id)
{
    uint64_t value = 0;

    if (!read_object_property(
            fd,
            colorop_id,
            DRM_MODE_OBJECT_COLOROP,
            "NEXT",
            value))
    {
        return 0;
    }

    return static_cast<uint32_t>(value);
}


/*
 * Discover one DRM plane color pipeline.

 *
 * Important:
 *
 * The DRM color-pipeline API requires unused colorops to be
 * explicitly bypassed in the atomic request.
 *
 * We therefore collect every colorop in the selected chain,
 * not only MULTIPLIER and CTM.
 */
static bool discover_color_pipeline(
    int fd,
    uint32_t crtc_id,
    color_pipeline_info& result)
{
    result =
        color_pipeline_info{};

    if (drmSetClientCap(
            fd,
            DRM_CLIENT_CAP_ATOMIC,
            1) != 0)
    {
        return false;
    }

    if (drmSetClientCap(
            fd,
            DRM_CLIENT_CAP_PLANE_COLOR_PIPELINE,
            1) != 0)
    {
        return false;
    }

    uint32_t plane_id =
        find_primary_plane(
            fd,
            crtc_id);

    if (plane_id == 0)
        return false;

    uint64_t current_pipeline = 0;

    uint32_t pipeline_property =
        find_object_property(
            fd,
            plane_id,
            DRM_MODE_OBJECT_PLANE,
            "COLOR_PIPELINE",
            &current_pipeline);

    if (pipeline_property == 0)
        return false;

    std::vector<uint64_t> pipelines =
        get_property_values(
            fd,
            pipeline_property);

    if (pipelines.empty())
        return false;

    for (uint64_t pipeline_value : pipelines)
    {
        if (pipeline_value == 0)
            continue;

        uint32_t first_colorop =
            static_cast<uint32_t>(
                pipeline_value);

        std::vector<colorop_info> ops;

        uint32_t multiplier_id = 0;
        uint32_t multiplier_property = 0;

        uint32_t ctm_id = 0;
        uint32_t ctm_data_property = 0;

        uint32_t colorop_id =
            first_colorop;

        bool valid_chain = true;

        for (uint32_t n = 0;
             n < MAX_COLOROP_CHAIN &&
             colorop_id != 0;
             ++n)
        {
            colorop_info op;

            op.id =
                colorop_id;

            op.type =
                get_colorop_type(
                    fd,
                    colorop_id);

            if (op.type == UINT32_MAX)
            {
                valid_chain = false;
                break;
            }

            op.bypass_property =
                find_object_property(
                    fd,
                    colorop_id,
                    DRM_MODE_OBJECT_COLOROP,
                    "BYPASS");

            if (op.type ==
                DRM_COLOROP_MULTIPLIER)
            {
                op.multiplier_property =
                    find_object_property(
                        fd,
                        colorop_id,
                        DRM_MODE_OBJECT_COLOROP,
                        "MULTIPLIER");

                if (op.multiplier_property != 0)
                {
                    multiplier_id =
                        colorop_id;

                    multiplier_property =
                        op.multiplier_property;
                }
            }
            else if (op.type ==
                     DRM_COLOROP_CTM_3X4)
            {
                op.data_property =
                    find_object_property(
                        fd,
                        colorop_id,
                        DRM_MODE_OBJECT_COLOROP,
                        "DATA");

                if (op.data_property != 0)
                {
                    ctm_id =
                        colorop_id;

                    ctm_data_property =
                        op.data_property;
                }
            }

            ops.push_back(op);

            colorop_id =
                get_colorop_next(
                    fd,
                    colorop_id);

            if (n + 1 == MAX_COLOROP_CHAIN &&
                colorop_id != 0)
            {
                valid_chain = false;
            }
        }

        if (!valid_chain)
            continue;

        if (ops.empty())
            continue;

        if (multiplier_id == 0 ||
            multiplier_property == 0)
        {
            continue;
        }

        if (ctm_id == 0 ||
            ctm_data_property == 0)
        {
            continue;
        }

        result.valid = true;

        result.plane_id =
            plane_id;

        result.color_pipeline_property =
            pipeline_property;

        result.original_pipeline =
            current_pipeline;

        result.selected_pipeline =
            pipeline_value;

        result.colorops =
            std::move(ops);

        result.multiplier_colorop =
            multiplier_id;

        result.multiplier_property =
            multiplier_property;

        result.ctm_colorop =
            ctm_id;

        result.ctm_data_property =
            ctm_data_property;

        return true;
    }

    return false;
}


/*
 * Add the complete color pipeline configuration to one
 * atomic request.
 *
 * Every colorop is bypassed first.
 *
 * Then:
 *
 *     MULTIPLIER = brightness
 *     CTM_3X4    = saturation matrix
 *
 * are enabled.
 */
static bool add_color_pipeline_properties(
    int fd,
    drmModeAtomicReq *request,
    const color_pipeline_info& pipeline,
    uint32_t ctm_blob_id,
    double brightness)
{
    if (!pipeline.valid ||
        !request ||
        pipeline.plane_id == 0)
    {
        return false;
    }

    if (drmModeAtomicAddProperty(
            request,
            pipeline.plane_id,
            pipeline.color_pipeline_property,
            pipeline.selected_pipeline) < 0)
    {
        return false;
    }

    /*
     * First put every colorop into bypass.
     *
     * This is important on AMD because the discovered pipeline
     * may also contain EOTF/OETF/LUT/3D-LUT blocks.
     */
    for (const auto& op : pipeline.colorops)
    {
        if (op.bypass_property == 0)
            continue;

        if (drmModeAtomicAddProperty(
                request,
                op.id,
                op.bypass_property,
                1) < 0)
        {
            return false;
        }
    }

    /*
     * Enable MULTIPLIER.
     */
    if (drmModeAtomicAddProperty(
            request,
            pipeline.multiplier_colorop,
            pipeline.colorops.empty() ?
                0 :
                pipeline.multiplier_property,
            double_to_drm_s31_32(
                brightness)) < 0)
    {
        return false;
    }

    uint32_t multiplier_bypass =
        find_object_property(
            fd,
            pipeline.multiplier_colorop,
            DRM_MODE_OBJECT_COLOROP,
            "BYPASS");

    if (multiplier_bypass)
    {
        if (drmModeAtomicAddProperty(
                request,
                pipeline.multiplier_colorop,
                multiplier_bypass,
                0) < 0)
        {
            return false;
        }
    }

    /*
     * Enable CTM.
     */
    uint32_t ctm_bypass =
        find_object_property(
            fd,
            pipeline.ctm_colorop,
            DRM_MODE_OBJECT_COLOROP,
            "BYPASS");

    if (ctm_bypass)
    {
        if (drmModeAtomicAddProperty(
                request,
                pipeline.ctm_colorop,
                ctm_bypass,
                0) < 0)
        {
            return false;
        }
    }

    if (drmModeAtomicAddProperty(
            request,
            pipeline.ctm_colorop,
            pipeline.ctm_data_property,
            ctm_blob_id) < 0)
    {
        return false;
    }

    return true;
}


/*
 * Atomic DRM plane color pipeline commit.
 */
static bool atomic_commit_color_pipeline(
    int fd,
    const color_pipeline_info& pipeline,
    double brightness,
    double saturation)
{
    if (!pipeline.valid)
        return false;

    brightness =
        std::clamp(
            std::isfinite(brightness) ?
                brightness :
                BRIGHTNESS_DEFAULT,
            BRIGHTNESS_MIN,
            BRIGHTNESS_MAX);

    saturation =
        std::clamp(
            std::isfinite(saturation) ?
                saturation :
                1.0,
            SATURATION_MIN,
            SATURATION_MAX);

    drm_color_ctm_3x4 ctm =
        make_saturation_ctm_3x4(
            saturation);

    drmModeAtomicReq *request =
        drmModeAtomicAlloc();

    if (!request)
        return false;

    uint32_t ctm_blob_id = 0;

    if (drmModeCreatePropertyBlob(
            fd,
            &ctm,
            sizeof(ctm),
            &ctm_blob_id) != 0)
    {
        drmModeAtomicFree(request);
        return false;
    }

    if (!add_color_pipeline_properties(
            fd,
            request,
            pipeline,
            ctm_blob_id,
            brightness))
    {
        int error = errno;

        drmModeDestroyPropertyBlob(
            fd,
            ctm_blob_id);

        drmModeAtomicFree(request);

        errno = error;

        return false;
    }

    /*
     * TEST_ONLY.
     */
    int ret =
        drmModeAtomicCommit(
            fd,
            request,
            DRM_MODE_ATOMIC_TEST_ONLY,
            nullptr);

    if (ret != 0)
    {
        int error = errno;

        drmModeDestroyPropertyBlob(
            fd,
            ctm_blob_id);

        drmModeAtomicFree(request);

        errno = error;

        return false;
    }

    /*
     * Real commit.
     */
    ret =
        drmModeAtomicCommit(
            fd,
            request,
            DRM_MODE_ATOMIC_NONBLOCK,
            nullptr);

    int error = errno;

    drmModeDestroyPropertyBlob(
        fd,
        ctm_blob_id);

    drmModeAtomicFree(request);

    errno = error;

    return ret == 0;
}


/*
 * Atomic CRTC CTM fallback.
 */
static bool atomic_commit_crtc_ctm(
    int fd,
    uint32_t crtc_id,
    uint32_t ctm_property,
    const drm_color_ctm *ctm)
{
    if (fd < 0 ||
        crtc_id == 0 ||
        ctm_property == 0)
    {
        return false;
    }

    drmModeAtomicReq *request =
        drmModeAtomicAlloc();

    if (!request)
        return false;

    uint32_t blob_id = 0;

    if (ctm)
    {
        if (drmModeCreatePropertyBlob(
                fd,
                ctm,
                sizeof(*ctm),
                &blob_id) != 0)
        {
            drmModeAtomicFree(request);
            return false;
        }
    }

    if (drmModeAtomicAddProperty(
            request,
            crtc_id,
            ctm_property,
            blob_id) < 0)
    {
        if (blob_id)
        {
            drmModeDestroyPropertyBlob(
                fd,
                blob_id);
        }

        drmModeAtomicFree(request);

        return false;
    }

    int ret =
        drmModeAtomicCommit(
            fd,
            request,
            DRM_MODE_ATOMIC_TEST_ONLY,
            nullptr);

    if (ret != 0)
    {
        int error = errno;

        if (blob_id)
        {
            drmModeDestroyPropertyBlob(
                fd,
                blob_id);
        }

        drmModeAtomicFree(request);

        errno = error;

        return false;
    }

    ret =
        drmModeAtomicCommit(
            fd,
            request,
            DRM_MODE_ATOMIC_NONBLOCK,
            nullptr);

    int error = errno;

    if (blob_id)
    {
        drmModeDestroyPropertyBlob(
            fd,
            blob_id);
    }

    drmModeAtomicFree(request);

    errno = error;

    return ret == 0;
}


/*
 * Wayfire owns this fd.
 *
 * Borrowed. Never close it.
 */
static int get_wayfire_drm_fd()
{
    auto& core =
        wf::get_core();

    if (!core.backend)
        return -1;

    return wlr_backend_get_drm_fd(
        core.backend);
}

} // namespace


class hw_saturation_output :
    public wf::custom_data_t
{
private:

    wf::output_t *output = nullptr;

    wf::option_wrapper_t<double>
        saturation_opt{
            "hw-saturation/value"
        };

    wf::option_wrapper_t<double>
        brightness_opt{
            "hw-saturation/brightness"
        };

    wl_event_source *idle_source = nullptr;

    wl_listener commit_listener{};
    wl_listener destroy_listener{};

    bool commit_listener_connected = false;
    bool destroy_listener_connected = false;

    bool scheduled = false;
    bool destroying = false;
    bool applying = false;

    int retry_count = 0;

    uint32_t current_crtc_id = 0;

    uint32_t original_ctm_property = 0;
    uint32_t original_crtc_id = 0;

    std::optional<drm_color_ctm>
        original_ctm;

    color_pipeline_info
        color_pipeline{};

    bool using_color_pipeline = false;


private:

    static hw_saturation_output *
    from_commit_listener(
        wl_listener *listener)
    {
        return wl_container_of(
            listener,
            static_cast<hw_saturation_output *>(nullptr),
            commit_listener);
    }


    static hw_saturation_output *
    from_destroy_listener(
        wl_listener *listener)
    {
        return wl_container_of(
            listener,
            static_cast<hw_saturation_output *>(nullptr),
            destroy_listener);
    }


    static void idle_apply(void *data)
    {
        auto *self =
            static_cast<hw_saturation_output *>(
                data);

        if (!self)
            return;

        self->idle_source = nullptr;
        self->scheduled = false;

        if (self->destroying)
            return;

        self->apply_saturation();
    }


    static int retry_apply(void *data)
    {
        auto *self =
            static_cast<hw_saturation_output *>(
                data);

        if (!self)
            return 0;

        self->idle_source = nullptr;
        self->scheduled = false;

        if (self->destroying)
            return 0;

        self->apply_saturation();

        return 0;
    }


    static void handle_output_commit(
        wl_listener *listener,
        void *data)
    {
        auto *self =
            from_commit_listener(
                listener);

        auto *event =
            static_cast<wlr_output_event_commit *>(
                data);

        if (!self ||
            !event ||
            self->destroying ||
            !self->output ||
            !self->output->handle)
        {
            return;
        }

        if (event->output !=
            self->output->handle)
        {
            return;
        }

        if (!event->state)
            return;

        /*
         * Do not react to COLOR_TRANSFORM.
         *
         * Our DRM property-only atomic commit must not cause
         * an endless self-triggered apply loop.
         */
        constexpr uint32_t relevant =
            WLR_OUTPUT_STATE_MODE |
            WLR_OUTPUT_STATE_ENABLED |
            WLR_OUTPUT_STATE_SCALE |
            WLR_OUTPUT_STATE_TRANSFORM |
            WLR_OUTPUT_STATE_ADAPTIVE_SYNC_ENABLED |
            WLR_OUTPUT_STATE_RENDER_FORMAT |
            WLR_OUTPUT_STATE_LAYERS |
            WLR_OUTPUT_STATE_IMAGE_DESCRIPTION;

        if (event->state->committed & relevant)
        {
            self->retry_count = 0;
            self->schedule_apply();
        }
    }


    static void handle_output_destroy(
        wl_listener *listener,
        void *data)
    {
        auto *self =
            from_destroy_listener(
                listener);

        (void)data;

        if (!self)
            return;

        self->destroying = true;
        self->output = nullptr;

        if (self->idle_source)
        {
            wl_event_source_remove(
                self->idle_source);

            self->idle_source = nullptr;
        }

        self->scheduled = false;

        if (self->commit_listener_connected)
        {
            wl_list_remove(
                &self->commit_listener.link);

            self->commit_listener_connected =
                false;
        }

        /*
         * Do NOT remove destroy_listener here.
         *
         * The current callback is executing from that listener.
         * It is removed by fini/destruction.
         */
    }


    void schedule_apply()
    {
        if (destroying ||
            scheduled ||
            !output ||
            !output->handle)
        {
            return;
        }

        auto& core =
            wf::get_core();

        if (!core.ev_loop)
            return;

        idle_source =
            wl_event_loop_add_idle(
                core.ev_loop,
                &hw_saturation_output::idle_apply,
                this);

        if (!idle_source)
        {
            std::fprintf(
                stderr,
                "[hw_saturation] "
                "ERROR: failed to schedule KMS update\n");

            return;
        }

        scheduled = true;
    }


    void schedule_retry()
    {
        if (destroying ||
            scheduled ||
            !output ||
            !output->handle)
        {
            return;
        }

        if (retry_count >=
            MAX_RETRY_COUNT)
        {
            std::fprintf(
                stderr,
                "[hw_saturation] "
                "ERROR: KMS color commit failed "
                "after %d attempts\n",
                MAX_RETRY_COUNT);

            return;
        }

        auto& core =
            wf::get_core();

        if (!core.ev_loop)
            return;

        idle_source =
            wl_event_loop_add_timer(
                core.ev_loop,
                &hw_saturation_output::retry_apply,
                this);

        if (!idle_source)
        {
            std::fprintf(
                stderr,
                "[hw_saturation] "
                "ERROR: failed to create retry timer\n");

            return;
        }

        ++retry_count;
        scheduled = true;

        wl_event_source_timer_update(
            idle_source,
            RETRY_DELAY_MS);
    }


    double get_saturation() const
    {
        double value =
            static_cast<double>(
                saturation_opt);

        if (!std::isfinite(value))
            value = 1.0;

        return std::clamp(
            value,
            SATURATION_MIN,
            SATURATION_MAX);
    }


    double get_brightness() const
    {
        double value =
            static_cast<double>(
                brightness_opt);

        if (!std::isfinite(value))
            value = BRIGHTNESS_DEFAULT;

        value = std::clamp(value, BRIGHTNESS_MIN, BRIGHTNESS_MAX);

        return 1.0 + ((value - 1.0) * 0.10);
    }


    bool is_drm_output() const
    {
        return
            output &&
            output->handle &&
            wlr_output_is_drm(
                output->handle);
    }


    bool discover_crtc(
        int fd,
        uint32_t& crtc_id)
    {
        crtc_id = 0;

        if (!output ||
            !output->handle)
        {
            return false;
        }

        if (!is_drm_output())
        {
            std::fprintf(
                stderr,
                "[hw_saturation] "
                "ERROR: output %s is not a DRM output\n",
                output->handle->name ?
                    output->handle->name :
                    "<unknown>");

            return false;
        }

        uint32_t connector_id =
            wlr_drm_connector_get_id(
                output->handle);

        if (connector_id == 0)
        {
            std::fprintf(
                stderr,
                "[hw_saturation] "
                "ERROR: could not obtain DRM connector "
                "ID for %s\n",
                output->handle->name ?
                    output->handle->name :
                    "<unknown>");

            return false;
        }

        crtc_id =
            find_crtc_for_connector(
                fd,
                connector_id);

        if (crtc_id == 0)
        {
            std::fprintf(
                stderr,
                "[hw_saturation] "
                "ERROR: no active CRTC for output=%s "
                "connector=%u\n",
                output->handle->name ?
                    output->handle->name :
                    "<unknown>",
                connector_id);

            return false;
        }

        return true;
    }


    bool discover_fallback_ctm(
        int fd,
        uint32_t crtc_id,
        uint32_t& ctm_property,
        uint64_t& current_ctm_blob)
    {
        ctm_property = 0;
        current_ctm_blob = 0;

        ctm_property =
            find_object_property(
                fd,
                crtc_id,
                DRM_MODE_OBJECT_CRTC,
                "CTM",
                &current_ctm_blob);

        if (ctm_property == 0)
        {
            std::fprintf(
                stderr,
                "[hw_saturation] "
                "ERROR: CRTC %u does not expose CTM\n",
                crtc_id);

            return false;
        }

        return true;
    }


    void remember_original_ctm(
        int fd,
        uint32_t crtc_id,
        uint32_t ctm_property,
        uint64_t current_ctm_blob)
    {
        if (original_crtc_id == crtc_id &&
            original_ctm_property ==
                ctm_property)
        {
            return;
        }

        original_crtc_id =
            crtc_id;

        original_ctm_property =
            ctm_property;

        original_ctm =
            read_ctm_blob(
                fd,
                static_cast<uint32_t>(
                    current_ctm_blob));
    }


    bool apply_color_pipeline(
        int fd,
        uint32_t crtc_id,
        double brightness,
        double saturation)
    {
        color_pipeline_info pipeline;

        if (!discover_color_pipeline(
                fd,
                crtc_id,
                pipeline))
        {
            return false;
        }

        if (!atomic_commit_color_pipeline(
                fd,
                pipeline,
                brightness,
                saturation))
        {
            int error = errno;

            std::fprintf(
                stderr,
                "[hw_saturation] "
                "ERROR: COLOR_PIPELINE commit failed: "
                "errno=%d (%s) "
                "plane=%u pipeline=%u "
                "multiplier=%u ctm=%u\n",
                error,
                std::strerror(error),
                pipeline.plane_id,
                static_cast<uint32_t>(
                    pipeline.selected_pipeline),
                pipeline.multiplier_colorop,
                pipeline.ctm_colorop);

            errno = error;

            return false;
        }

        color_pipeline =
            pipeline;

        using_color_pipeline = true;

        std::fprintf(
            stderr,
            "[hw_saturation] "
            "SUCCESS: COLOR_PIPELINE committed: "
            "output=%s "
            "crtc=%u "
            "plane=%u "
            "pipeline=%u "
            "colorops=%zu "
            "multiplier=%u "
            "ctm=%u "
            "saturation=%.6f "
            "brightness=%.6f\n",
            output->handle->name ?
                output->handle->name :
                "<unknown>",
            crtc_id,
            pipeline.plane_id,
            static_cast<uint32_t>(
                pipeline.selected_pipeline),
            pipeline.colorops.size(),
            pipeline.multiplier_colorop,
            pipeline.ctm_colorop,
            saturation,
            brightness);

        return true;
    }


    bool apply_fallback_ctm(
        int fd,
        uint32_t crtc_id,
        double brightness,
        double saturation)
    {
        uint32_t ctm_property = 0;
        uint64_t current_ctm_blob = 0;

        if (!discover_fallback_ctm(
                fd,
                crtc_id,
                ctm_property,
                current_ctm_blob))
        {
            return false;
        }

        remember_original_ctm(
            fd,
            crtc_id,
            ctm_property,
            current_ctm_blob);

        drm_color_ctm ctm =
            make_saturation_ctm(
                saturation,
                brightness);

        if (!atomic_commit_crtc_ctm(
                fd,
                crtc_id,
                ctm_property,
                &ctm))
        {
            int error = errno;

            std::fprintf(
                stderr,
                "[hw_saturation] "
                "ERROR: fallback CRTC CTM commit failed: "
                "errno=%d (%s)\n",
                error,
                std::strerror(error));

            errno = error;

            return false;
        }

        using_color_pipeline = false;

        std::fprintf(
            stderr,
            "[hw_saturation] "
            "SUCCESS: CRTC CTM committed: "
            "output=%s "
            "crtc=%u "
            "saturation=%.6f "
            "brightness=%.6f\n",
            output->handle->name ?
                output->handle->name :
                "<unknown>",
            crtc_id,
            saturation,
            brightness);

        return true;
    }


    bool apply_saturation()
    {
        if (destroying ||
            applying ||
            !output ||
            !output->handle)
        {
            return false;
        }

        applying = true;

        const double saturation =
            get_saturation();

        const double brightness =
            get_brightness();

        int fd =
            get_wayfire_drm_fd();

        if (fd < 0)
        {
            std::fprintf(
                stderr,
                "[hw_saturation] "
                "ERROR: Wayfire DRM fd unavailable\n");

            applying = false;

            schedule_retry();

            return false;
        }

        if (drmSetClientCap(
                fd,
                DRM_CLIENT_CAP_ATOMIC,
                1) != 0)
        {
            int error = errno;

            std::fprintf(
                stderr,
                "[hw_saturation] "
                "ERROR: DRM atomic KMS unavailable: "
                "errno=%d (%s)\n",
                error,
                std::strerror(error));

            applying = false;

            schedule_retry();

            return false;
        }

        uint32_t crtc_id = 0;

        if (!discover_crtc(
                fd,
                crtc_id))
        {
            applying = false;

            schedule_retry();

            return false;
        }

        current_crtc_id =
            crtc_id;

        /*
         * First use the DRM plane color pipeline.
         */
        if (apply_color_pipeline(
                fd,
                crtc_id,
                brightness,
                saturation))
        {
            retry_count = 0;
            applying = false;

            return true;
        }

        /*
         * Fallback to CRTC CTM.
         */
        if (apply_fallback_ctm(
                fd,
                crtc_id,
                brightness,
                saturation))
        {
            retry_count = 0;
            applying = false;

            return true;
        }

        applying = false;

        schedule_retry();

        return false;
    }


    void restore_original()
    {
        if (!output ||
            !output->handle)
        {
            return;
        }

        int fd =
            get_wayfire_drm_fd();

        if (fd < 0)
            return;

        /*
         * Restore the plane pipeline.
         */
        if (using_color_pipeline &&
            color_pipeline.valid)
        {
            drmModeAtomicReq *request =
                drmModeAtomicAlloc();

            if (request)
            {
                if (drmModeAtomicAddProperty(
                        request,
                        color_pipeline.plane_id,
                        color_pipeline.color_pipeline_property,
                        color_pipeline.original_pipeline) >= 0)
                {
                    int ret =
                        drmModeAtomicCommit(
                            fd,
                            request,
                            DRM_MODE_ATOMIC_TEST_ONLY,
                            nullptr);

                    if (ret == 0)
                    {
                        ret =
                            drmModeAtomicCommit(
                                fd,
                                request,
                                DRM_MODE_ATOMIC_NONBLOCK,
                                nullptr);
                    }

                    if (ret != 0)
                    {
                        int error = errno;

                        std::fprintf(
                            stderr,
                            "[hw_saturation] "
                            "WARNING: failed to restore "
                            "COLOR_PIPELINE: "
                            "errno=%d (%s)\n",
                            error,
                            std::strerror(error));
                    }
                    else
                    {
                        std::fprintf(
                            stderr,
                            "[hw_saturation] "
                            "Original COLOR_PIPELINE restored "
                            "for %s\n",
                            output->handle->name ?
                                output->handle->name :
                                "<unknown>");
                    }
                }

                drmModeAtomicFree(request);
            }
        }

        /*
         * Restore original CRTC CTM.
         */
        if (original_crtc_id != 0 &&
            original_ctm_property != 0)
        {
            const drm_color_ctm *ctm_ptr =
                nullptr;

            if (original_ctm.has_value())
                ctm_ptr =
                    &*original_ctm;

            if (!atomic_commit_crtc_ctm(
                    fd,
                    original_crtc_id,
                    original_ctm_property,
                    ctm_ptr))
            {
                int error = errno;

                std::fprintf(
                    stderr,
                    "[hw_saturation] "
                    "WARNING: failed to restore "
                    "original CRTC CTM for %s: "
                    "errno=%d (%s)\n",
                    output->handle->name ?
                        output->handle->name :
                        "<unknown>",
                    error,
                    std::strerror(error));
            }
            else
            {
                std::fprintf(
                    stderr,
                    "[hw_saturation] "
                    "Original CRTC CTM restored for %s\n",
                    output->handle->name ?
                        output->handle->name :
                        "<unknown>");
            }
        }

        using_color_pipeline = false;
        color_pipeline =
            color_pipeline_info{};
    }


public:

    explicit hw_saturation_output(
        wf::output_t *wo) :
        output(wo)
    {
        if (!output ||
            !output->handle)
        {
            return;
        }

        commit_listener.notify =
            &hw_saturation_output::handle_output_commit;

        destroy_listener.notify =
            &hw_saturation_output::handle_output_destroy;

        wl_signal_add(
            &output->handle->events.commit,
            &commit_listener);

        commit_listener_connected =
            true;

        wl_signal_add(
            &output->handle->events.destroy,
            &destroy_listener);

        destroy_listener_connected =
            true;
    }


    ~hw_saturation_output()
    {
        fini();
    }


    void init()
    {
        saturation_opt.set_callback(
            [this]()
            {
                retry_count = 0;
                schedule_apply();
            });

        brightness_opt.set_callback(
            [this]()
            {
                retry_count = 0;
                schedule_apply();
            });

        schedule_apply();
    }


    void fini()
    {
        if (destroying)
            return;

        destroying = true;

        if (idle_source)
        {
            wl_event_source_remove(
                idle_source);

            idle_source = nullptr;
        }

        scheduled = false;

        /*
         * Output is still valid here when fini() is called
         * normally from custom_data destruction.
         */
        if (output &&
            output->handle)
        {
            restore_original();
        }

        if (commit_listener_connected)
        {
            wl_list_remove(
                &commit_listener.link);

            commit_listener_connected =
                false;
        }

        if (destroy_listener_connected)
        {
            wl_list_remove(
                &destroy_listener.link);

            destroy_listener_connected =
                false;
        }

        output = nullptr;
    }
};


class hw_saturation :
    public wf::plugin_interface_t,
    public wf::per_output_tracker_mixin_t<>
{
public:

    void init() override
    {
        init_output_tracking();
    }


    void fini() override
    {
        fini_output_tracking();
    }


    void handle_new_output(
        wf::output_t *output) override
    {
        output->store_data(
            std::make_unique<
                hw_saturation_output>(
                output));

        auto instance =
            output->get_data<
                hw_saturation_output>();

        if (instance)
            instance->init();
    }


    void handle_output_removed(
        wf::output_t *output) override
    {
        output->erase_data<
            hw_saturation_output>();
    }
};


DECLARE_WAYFIRE_PLUGIN(hw_saturation);

