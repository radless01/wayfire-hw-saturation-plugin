#include <wayfire/plugin.hpp>
#include <wayfire/per-output-plugin.hpp>
#include <wayfire/output.hpp>
#include <wayfire/option-wrapper.hpp>
#include <wayfire/core.hpp>
#include <wayfire/object.hpp>

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
#include <limits>
#include <memory>
#include <optional>
#include <unistd.h>


namespace
{

constexpr double RED_WEIGHT   = 0.2126;
constexpr double GREEN_WEIGHT = 0.7152;
constexpr double BLUE_WEIGHT  = 0.0722;

constexpr double SATURATION_MIN = 0.0;
constexpr double SATURATION_MAX = 3.0;

constexpr int MAX_RETRY_COUNT = 20;
constexpr int RETRY_DELAY_MS = 50;


/*
 * DRM uses S31.32 signed-magnitude for CTM values.
 *
 * Bit 63:
 *      sign
 *
 * Bits 62..32:
 *      integer magnitude
 *
 * Bits 31..0:
 *      fractional magnitude
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

    const bool negative =
        value < 0.0;

    if (negative)
        value = -value;

    const long double scaled =
        static_cast<long double>(value) *
        static_cast<long double>(uint64_t{1} << 32);

    uint64_t magnitude;

    if (scaled >=
        static_cast<long double>(
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
 * Saturation matrix:
 *
 * L = 0.2126 R + 0.7152 G + 0.0722 B
 *
 * output = L + S * (input - L)
 *
 * S = 0.0 -> grayscale
 * S = 1.0 -> identity
 * S > 1.0 -> increased saturation
 */
static drm_color_ctm make_saturation_ctm(
    double saturation)
{
    saturation =
        std::clamp(
            saturation,
            SATURATION_MIN,
            SATURATION_MAX);

    const double inverse =
        1.0 - saturation;

    drm_color_ctm ctm{};

    ctm.matrix[0] =
        double_to_drm_s31_32(
            inverse * RED_WEIGHT +
            saturation);

    ctm.matrix[1] =
        double_to_drm_s31_32(
            inverse * GREEN_WEIGHT);

    ctm.matrix[2] =
        double_to_drm_s31_32(
            inverse * BLUE_WEIGHT);


    ctm.matrix[3] =
        double_to_drm_s31_32(
            inverse * RED_WEIGHT);

    ctm.matrix[4] =
        double_to_drm_s31_32(
            inverse * GREEN_WEIGHT +
            saturation);

    ctm.matrix[5] =
        double_to_drm_s31_32(
            inverse * BLUE_WEIGHT);


    ctm.matrix[6] =
        double_to_drm_s31_32(
            inverse * RED_WEIGHT);

    ctm.matrix[7] =
        double_to_drm_s31_32(
            inverse * GREEN_WEIGHT);

    ctm.matrix[8] =
        double_to_drm_s31_32(
            inverse * BLUE_WEIGHT +
            saturation);

    return ctm;
}


/*
 * Find the CRTC currently associated with a DRM connector.
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


    /*
     * Current encoder.
     */
    if (connector->encoder_id)
    {
        drmModeEncoder *encoder =
            drmModeGetEncoder(
                fd,
                connector->encoder_id);

        if (encoder)
        {
            const uint32_t crtc =
                encoder->crtc_id;

            drmModeFreeEncoder(encoder);
            drmModeFreeConnector(connector);

            if (crtc)
                return crtc;
        }
    }


    /*
     * Search connector encoders.
     */
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

        const uint32_t crtc =
            encoder->crtc_id;

        drmModeFreeEncoder(encoder);

        if (crtc)
        {
            drmModeFreeConnector(connector);
            return crtc;
        }
    }


    /*
     * Search possible active CRTCs.
     */
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

        const uint32_t possible_crtcs =
            encoder->possible_crtcs;

        drmModeFreeEncoder(encoder);


        for (int i = 0;
             i < resources->count_crtcs;
             ++i)
        {
            if (!(possible_crtcs & (1u << i)))
                continue;

            const uint32_t crtc_id =
                resources->crtcs[i];

            drmModeCrtc *crtc =
                drmModeGetCrtc(
                    fd,
                    crtc_id);

            if (!crtc)
                continue;

            const bool active =
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
 * Find a property attached to a CRTC.
 */
static uint32_t find_crtc_property(
    int fd,
    uint32_t crtc_id,
    const char *name,
    uint64_t *current_value = nullptr)
{
    if (current_value)
        *current_value = 0;


    drmModeObjectProperties *properties =
        drmModeObjectGetProperties(
            fd,
            crtc_id,
            DRM_MODE_OBJECT_CRTC);

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
 * Read an existing CTM blob.
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


    if (blob->length >=
        sizeof(drm_color_ctm))
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
 * Atomic CRTC CTM commit.
 *
 * First TEST_ONLY.
 * Then real NONBLOCK commit.
 *
 * No modeset.
 */
static bool atomic_commit_ctm(
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


    /*
     * Validate without modifying hardware.
     */
    int ret =
        drmModeAtomicCommit(
            fd,
            request,
            DRM_MODE_ATOMIC_TEST_ONLY,
            nullptr);

    if (ret != 0)
    {
        const int error = errno;

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


    /*
     * Actual property-only commit.
     */
    ret =
        drmModeAtomicCommit(
            fd,
            request,
            DRM_MODE_ATOMIC_NONBLOCK,
            nullptr);

    const int error = errno;


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
 * Wayfire owns the DRM fd.
 *
 * The returned fd is borrowed.
 * NEVER close it here.
 */
static int get_wayfire_drm_fd()
{
    auto &core =
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
            "hw_saturation/value"
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


    /*
     * CRTC currently controlled by us.
     */
    uint32_t current_crtc_id = 0;


    /*
     * Original CRTC state.
     */
    uint32_t original_crtc_id = 0;
    uint32_t original_ctm_property = 0;

    std::optional<drm_color_ctm>
        original_ctm;


private:

    static hw_saturation_output *
    from_commit_listener(
        wl_listener *listener)
    {
        return reinterpret_cast<
            hw_saturation_output *>(
            reinterpret_cast<char *>(
                listener) -
            offsetof(
                hw_saturation_output,
                commit_listener));
    }


    static hw_saturation_output *
    from_destroy_listener(
        wl_listener *listener)
    {
        return reinterpret_cast<
            hw_saturation_output *>(
            reinterpret_cast<char *>(
                listener) -
            offsetof(
                hw_saturation_output,
                destroy_listener));
    }


    static void idle_apply(void *data)
    {
        auto *self =
            static_cast<
                hw_saturation_output *>(data);

        if (!self)
            return;


        self->idle_source = nullptr;
        self->scheduled = false;


        if (self->destroying)
            return;


        self->apply_saturation();
    }


    /*
     * wl_event_loop_add_timer() requires:
     *
     *      int (*)(void *)
     *
     * Returning 0 keeps the timer source alive.
     *
     * We remove the timer source ourselves immediately after
     * it fires by clearing idle_source/scheduled.
     */
    static int retry_apply(void *data)
    {
        auto *self =
            static_cast<
                hw_saturation_output *>(data);

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
            from_commit_listener(listener);


        auto *event =
            static_cast<
                wlr_output_event_commit *>(data);


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
         * IMPORTANT:
         *
         * Do NOT use WLR_OUTPUT_STATE_COLOR_REPRESENTATION.
         * It does not exist in the wlroots 0.20 headers used by
         * Wayfire 0.11.
         */
        constexpr uint32_t relevant =
            WLR_OUTPUT_STATE_MODE |
            WLR_OUTPUT_STATE_ENABLED |
            WLR_OUTPUT_STATE_SCALE |
            WLR_OUTPUT_STATE_TRANSFORM |
            WLR_OUTPUT_STATE_ADAPTIVE_SYNC_ENABLED |
            WLR_OUTPUT_STATE_RENDER_FORMAT |
            WLR_OUTPUT_STATE_LAYERS |
            WLR_OUTPUT_STATE_COLOR_TRANSFORM |
            WLR_OUTPUT_STATE_IMAGE_DESCRIPTION;


        if (event->state->committed &
            relevant)
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
            from_destroy_listener(listener);

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


        if (self->destroy_listener_connected)
        {
            wl_list_remove(
                &self->destroy_listener.link);

            self->destroy_listener_connected =
                false;
        }
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


        auto &core =
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
                "ERROR: KMS CTM failed after "
                "%d attempts\n",
                MAX_RETRY_COUNT);

            return;
        }


        auto &core =
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


    bool is_drm_output() const
    {
        return
            output &&
            output->handle &&
            wlr_output_is_drm(
                output->handle);
    }


    bool discover_kms(
        int fd,
        uint32_t& crtc_id,
        uint32_t& ctm_property,
        uint64_t& current_ctm_blob)
    {
        crtc_id = 0;
        ctm_property = 0;
        current_ctm_blob = 0;


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


        const uint32_t connector_id =
            wlr_drm_connector_get_id(
                output->handle);


        if (connector_id == 0)
        {
            std::fprintf(
                stderr,
                "[hw_saturation] "
                "ERROR: could not obtain DRM "
                "connector ID for %s\n",
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
                "ERROR: no active CRTC for "
                "output=%s connector=%u\n",
                output->handle->name ?
                    output->handle->name :
                    "<unknown>",
                connector_id);

            return false;
        }


        ctm_property =
            find_crtc_property(
                fd,
                crtc_id,
                "CTM",
                &current_ctm_blob);


        if (ctm_property == 0)
        {
            std::fprintf(
                stderr,
                "[hw_saturation] "
                "ERROR: CRTC %u does not expose "
                "a DRM CTM property\n",
                crtc_id);

            return false;
        }


        return true;
    }


    void remember_original_state(
        int fd,
        uint32_t crtc_id,
        uint32_t ctm_property,
        uint64_t current_blob)
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
                    current_blob));
    }


    bool commit_saturation(
        int fd,
        uint32_t crtc_id,
        uint32_t ctm_property,
        double saturation)
    {
        const drm_color_ctm ctm =
            make_saturation_ctm(
                saturation);


        return atomic_commit_ctm(
            fd,
            crtc_id,
            ctm_property,
            &ctm);
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


        const char *name =
            output->handle->name ?
                output->handle->name :
                "<unknown>";


        const double saturation =
            get_saturation();


        const int fd =
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


        /*
         * Wayfire/wlroots normally already enabled this.
         *
         * Setting the client capability here is harmless and makes
         * the direct libdrm path explicit.
         */
        if (drmSetClientCap(
                fd,
                DRM_CLIENT_CAP_ATOMIC,
                1) != 0)
        {
            const int error = errno;


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
        uint32_t ctm_property = 0;
        uint64_t current_ctm_blob = 0;


        if (!discover_kms(
                fd,
                crtc_id,
                ctm_property,
                current_ctm_blob))
        {
            applying = false;

            schedule_retry();

            return false;
        }


        remember_original_state(
            fd,
            crtc_id,
            ctm_property,
            current_ctm_blob);


        std::fprintf(
            stderr,
            "[hw_saturation] "
            "APPLY KMS CTM: "
            "output=%s "
            "connector=%u "
            "crtc=%u "
            "ctm_property=%u "
            "saturation=%.6f\n",
            name,
            wlr_drm_connector_get_id(
                output->handle),
            crtc_id,
            ctm_property,
            saturation);


        if (!commit_saturation(
                fd,
                crtc_id,
                ctm_property,
                saturation))
        {
            const int error = errno;


            std::fprintf(
                stderr,
                "[hw_saturation] "
                "ERROR: CTM atomic commit failed: "
                "errno=%d (%s)\n",
                error,
                std::strerror(error));


            applying = false;

            schedule_retry();

            return false;
        }


        current_crtc_id =
            crtc_id;

        retry_count = 0;


        std::fprintf(
            stderr,
            "[hw_saturation] "
            "SUCCESS: hardware KMS CTM committed: "
            "output=%s "
            "crtc=%u "
            "saturation=%.6f\n",
            name,
            crtc_id,
            saturation);


        applying = false;

        return true;
    }


    void restore_original()
    {
        if (!output ||
            !output->handle)
        {
            return;
        }


        if (current_crtc_id == 0)
            return;


        const int fd =
            get_wayfire_drm_fd();


        if (fd < 0)
            return;


        const uint32_t property =
            find_crtc_property(
                fd,
                current_crtc_id,
                "CTM");


        if (property == 0)
            return;


        drmModeCrtc *crtc =
            drmModeGetCrtc(
                fd,
                current_crtc_id);


        if (!crtc)
            return;


        const bool inactive =
            !crtc->mode_valid ||
            crtc->buffer_id == 0;


        drmModeFreeCrtc(crtc);


        if (inactive)
            return;


        bool restored = false;


        if (original_ctm.has_value())
        {
            restored =
                atomic_commit_ctm(
                    fd,
                    current_crtc_id,
                    property,
                    &*original_ctm);
        }
        else
        {
            /*
             * NULL CTM blob means unit/pass-through matrix.
             */
            restored =
                atomic_commit_ctm(
                    fd,
                    current_crtc_id,
                    property,
                    nullptr);
        }


        if (!restored)
        {
            const int error = errno;


            std::fprintf(
                stderr,
                "[hw_saturation] "
                "WARNING: failed to restore original CTM "
                "for %s: errno=%d (%s)\n",
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
                "Original CTM restored for %s\n",
                output->handle->name ?
                    output->handle->name :
                    "<unknown>");
        }
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
         * Restore before removing the listeners.
         */
        restore_original();


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


        /*
         * Wayfire 0.11 returns nonstd::observer_ptr<T>
         * from get_data(), not T*.
         */
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

