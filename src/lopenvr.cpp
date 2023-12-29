#include <prosper_command_buffer.hpp>
#include <prosper_fence.hpp>
#include "stdafx_openvr.h"
#include <optional>
#include <unordered_map>
#include <pragma/lua/classes/ldef_color.h>
#include <sharedutils/datastream.h>
#include <pragma/lua/libraries/c_lua_vulkan.h>
#include <pragma/lua/lua_entity_component.hpp>
#include <pragma/audio/e_alstate.h>
#include <pragma/game/c_game.h>
#include <pragma/physics/physobj.h>
#include <pragma/physics/collision_object.hpp>
#include <pragma/lua/c_lentity_handles.hpp>
#include <pragma/lua/converters/game_type_converters_t.hpp>
#include <pragma/entities/environment/c_env_camera.h>
#include <pragma/entities/components/c_scene_component.hpp>
#include <pragma/c_engine.h>
#include <luainterface.hpp>
#include <sharedutils/functioncallback.h>
#include <pragma/pragma_module.hpp>
#include <pragma/util/util_game.hpp>
#include "vr_instance.hpp"
#include "vr_eye.hpp"
#include "lopenvr.h"
#include "wvmodule.h"
#include <glm/gtx/matrix_decompose.hpp>

std::unique_ptr<openvr::Instance> s_vrInstance = nullptr;

static openvr::Eye &get_eye(lua_State *l, int32_t eyeIdIndex)
{
	auto eyeId = static_cast<vr::EVREye>(Lua::CheckInt(l, eyeIdIndex));
	switch(eyeId) {
	case vr::EVREye::Eye_Left:
		return s_vrInstance->GetLeftEye();
	default:
		return s_vrInstance->GetRightEye();
	}
}

extern "C" {
PRAGMA_EXPORT void openvr_set_hmd_view_enabled(bool b)
{
	if(s_vrInstance == nullptr)
		return;
	s_vrInstance->SetHmdViewEnabled(b);
}
PRAGMA_EXPORT void openvr_set_controller_state_callback(const std::function<void(uint32_t, uint32_t, GLFW::KeyState)> &f)
{
	if(s_vrInstance == nullptr)
		return;
	s_vrInstance->SetControllerStateCallback(f);
}
PRAGMA_EXPORT void openvr_set_mirror_window_enabled(bool b)
{
	if(s_vrInstance == nullptr)
		return;
	if(b == true)
		s_vrInstance->ShowMirrorWindow();
	else
		s_vrInstance->HideMirrorWindow();
}

PRAGMA_EXPORT bool openvr_initialize(std::string &strErr, std::vector<std::string> &reqInstanceExtensions, std::vector<std::string> &reqDeviceExtensions)
{
	if(s_vrInstance != nullptr)
		return true;
	vr::EVRInitError err;
#if LOPENVR_VERBOSE == 1
	std::cout << "[VR] Creating vr instance..." << std::endl;
#endif
	s_vrInstance = openvr::Instance::Create(&err, reqInstanceExtensions, reqDeviceExtensions);
	strErr = openvr::to_string(err);
	if(s_vrInstance == nullptr)
		return false;
#if LOPENVR_VERBOSE == 1
	std::cout << "[VR] Initializing vr instance..." << std::endl;
#endif
	auto r = (err == vr::EVRInitError::VRInitError_None) ? true : false;
	if(r == true)
		s_vrInstance->HideMirrorWindow();
#if LOPENVR_VERBOSE == 1
	std::cout << "[VR] Initialization complete!" << std::endl;
#endif
	return r;
}
};

int run_openxr_demo(int argc, char *argv[]);
#include <prosper_window.hpp>
GLFW::Window *get_glfw_window() { return &*pragma::get_cengine()->GetRenderContext().GetWindow(); }

//#include "openxr/pvr_openxr_instance.hpp"
int Lua::openvr::lib::initialize(lua_State *l)
{
	//static auto instance = pvr::XrInstance::Create();
	//for(;;)
	//	instance->Render();
	//return 0;
	vr::EVRInitError err;
	if(s_vrInstance != nullptr)
		err = vr::EVRInitError::VRInitError_None;
	else {
		std::vector<std::string> reqInstanceExtensions;
		std::vector<std::string> reqDeviceExtensions;
		s_vrInstance = ::openvr::Instance::Create(&err, reqInstanceExtensions, reqDeviceExtensions);
	}
	Lua::PushInt(l, static_cast<uint32_t>(err));
	return 1;
}

int Lua::openvr::lib::close(lua_State *l)
{
	s_vrInstance = nullptr;
	return 0;
}

int Lua::openvr::lib::property_error_to_string(lua_State *l)
{
	auto err = Lua::CheckInt(l, 1);
	Lua::PushString(l, ::openvr::to_string(static_cast<vr::ETrackedPropertyError>(err)));
	return 1;
}

int Lua::openvr::lib::init_error_to_string(lua_State *l)
{
	auto err = Lua::CheckInt(l, 1);
	Lua::PushString(l, ::openvr::to_string(static_cast<vr::EVRInitError>(err)));
	return 1;
}

int Lua::openvr::lib::compositor_error_to_string(lua_State *l)
{
	auto err = Lua::CheckInt(l, 1);
	Lua::PushString(l, ::openvr::to_string(static_cast<vr::VRCompositorError>(err)));
	return 1;
}

int Lua::openvr::lib::button_id_to_string(lua_State *l)
{
	if(s_vrInstance == nullptr)
		return 0;
	auto *sys = s_vrInstance->GetSystemInterface();
	auto buttonId = Lua::CheckInt(l, 1);
	Lua::PushString(l, sys->GetButtonIdNameFromEnum(static_cast<vr::EVRButtonId>(buttonId)));
	return 1;
}

int Lua::openvr::lib::event_type_to_string(lua_State *l)
{
	if(s_vrInstance == nullptr)
		return 0;
	auto *sys = s_vrInstance->GetSystemInterface();
	auto evType = Lua::CheckInt(l, 1);
	Lua::PushString(l, ::openvr::to_string(static_cast<vr::EVREventType>(evType)));
	return 1;
}

int Lua::openvr::lib::controller_axis_type_to_string(lua_State *l)
{
	if(s_vrInstance == nullptr)
		return 0;
	auto *sys = s_vrInstance->GetSystemInterface();
	auto controllerAxisType = Lua::CheckInt(l, 1);
	Lua::PushString(l, sys->GetControllerAxisTypeNameFromEnum(static_cast<vr::EVRControllerAxisType>(controllerAxisType)));
	return 1;
}

template<class T>
static int get_property(lua_State *l, const std::function<T(vr::TrackedPropertyError *)> &f, const std::function<void(lua_State *, T &)> &push)
{
	int32_t numArgs = 1;
	if(s_vrInstance == nullptr)
		Lua::PushInt(l, vr::ETrackedPropertyError::TrackedProp_InvalidDevice);
	else {
		vr::TrackedPropertyError err;
		auto r = f(&err);
		Lua::PushInt(l, static_cast<uint32_t>(err));
		if(err == vr::ETrackedPropertyError::TrackedProp_Success) {
			push(l, r);
			++numArgs;
		}
	}
	return numArgs;
}

static int get_property_string(lua_State *l, const std::function<std::string(vr::TrackedPropertyError *)> &f)
{
	return get_property<std::string>(l, f, [](lua_State *l, std::string &str) -> void { Lua::PushString(l, str); });
}
static int get_property_bool(lua_State *l, const std::function<bool(vr::TrackedPropertyError *)> &f)
{
	return get_property<bool>(l, f, [](lua_State *l, bool &b) -> void { Lua::PushBool(l, b); });
}
static int get_property_float(lua_State *l, const std::function<float(vr::TrackedPropertyError *)> &f)
{
	return get_property<float>(l, f, [](lua_State *l, float &f) -> void { Lua::PushNumber(l, f); });
}
static int get_property_mat3x4(lua_State *l, const std::function<glm::mat3x4(vr::TrackedPropertyError *)> &f)
{
	return get_property<glm::mat3x4>(l, f, [](lua_State *l, glm::mat3x4 &m) -> void { Lua::Push<glm::mat3x4>(l, m); });
}
static int get_property_int32(lua_State *l, const std::function<int32_t(vr::TrackedPropertyError *)> &f)
{
	return get_property<int32_t>(l, f, [](lua_State *l, int32_t &i) -> void { Lua::PushInt(l, i); });
}
static int get_property_uint64(lua_State *l, const std::function<uint64_t(vr::TrackedPropertyError *)> &f)
{
	return get_property<uint64_t>(l, f, [](lua_State *l, uint64_t &i) -> void { Lua::PushInt(l, i); });
}
static int get_property_vec2(lua_State *l, const std::function<glm::vec2(vr::TrackedPropertyError *)> &f)
{
	return get_property<glm::vec2>(l, f, [](lua_State *l, glm::vec2 &v) -> void { Lua::Push<glm::vec2>(l, v); });
}

int Lua::openvr::lib::get_tracking_system_name(lua_State *l)
{
	return get_property_string(l, [](vr::TrackedPropertyError *err) -> std::string { return s_vrInstance->GetTrackingSystemName(err); });
}
int Lua::openvr::lib::get_model_number(lua_State *l)
{
	return get_property_string(l, [](vr::TrackedPropertyError *err) -> std::string { return s_vrInstance->GetModelNumber(err); });
}
int Lua::openvr::lib::get_serial_number(lua_State *l)
{
	return get_property_string(l, [](vr::TrackedPropertyError *err) -> std::string { return s_vrInstance->GetSerialNumber(err); });
}

int Lua::openvr::lib::get_render_model_name(lua_State *l)
{
	return get_property_string(l, [](vr::TrackedPropertyError *err) -> std::string { return s_vrInstance->GetRenderModelName(err); });
}
int Lua::openvr::lib::get_manufacturer_name(lua_State *l)
{
	return get_property_string(l, [](vr::TrackedPropertyError *err) -> std::string { return s_vrInstance->GetManufacturerName(err); });
}
int Lua::openvr::lib::get_tracking_firmware_version(lua_State *l)
{
	return get_property_string(l, [](vr::TrackedPropertyError *err) -> std::string { return s_vrInstance->GetTrackingFirmwareVersion(err); });
}
int Lua::openvr::lib::get_hardware_revision(lua_State *l)
{
	return get_property_string(l, [](vr::TrackedPropertyError *err) -> std::string { return s_vrInstance->GetHardwareRevision(err); });
}
int Lua::openvr::lib::get_all_wireless_dongle_descriptions(lua_State *l)
{
	return get_property_string(l, [](vr::TrackedPropertyError *err) -> std::string { return s_vrInstance->GetAllWirelessDongleDescriptions(err); });
}
int Lua::openvr::lib::get_connected_wireless_dongle(lua_State *l)
{
	return get_property_string(l, [](vr::TrackedPropertyError *err) -> std::string { return s_vrInstance->GetConnectedWirelessDongle(err); });
}
int Lua::openvr::lib::get_firmware_manual_update_url(lua_State *l)
{
	return get_property_string(l, [](vr::TrackedPropertyError *err) -> std::string { return s_vrInstance->GetFirmwareManualUpdateURL(err); });
}
int Lua::openvr::lib::get_firmware_programming_target(lua_State *l)
{
	return get_property_string(l, [](vr::TrackedPropertyError *err) -> std::string { return s_vrInstance->GetFirmwareProgrammingTarget(err); });
}
int Lua::openvr::lib::get_display_mc_image_left(lua_State *l)
{
	return get_property_string(l, [](vr::TrackedPropertyError *err) -> std::string { return s_vrInstance->GetDisplayMCImageLeft(err); });
}
int Lua::openvr::lib::get_display_mc_image_right(lua_State *l)
{
	return get_property_string(l, [](vr::TrackedPropertyError *err) -> std::string { return s_vrInstance->GetDisplayMCImageRight(err); });
}
int Lua::openvr::lib::get_display_gc_image(lua_State *l)
{
	return get_property_string(l, [](vr::TrackedPropertyError *err) -> std::string { return s_vrInstance->GetDisplayGCImage(err); });
}
int Lua::openvr::lib::get_camera_firmware_description(lua_State *l)
{
	return get_property_string(l, [](vr::TrackedPropertyError *err) -> std::string { return s_vrInstance->GetCameraFirmwareDescription(err); });
}
int Lua::openvr::lib::get_attached_device_id(lua_State *l)
{
	return get_property_string(l, [](vr::TrackedPropertyError *err) -> std::string { return s_vrInstance->GetAttachedDeviceId(err); });
}
int Lua::openvr::lib::get_model_label(lua_State *l)
{
	return get_property_string(l, [](vr::TrackedPropertyError *err) -> std::string { return s_vrInstance->GetModelLabel(err); });
}

int Lua::openvr::lib::will_drift_in_yaw(lua_State *l)
{
	return get_property_bool(l, [](vr::TrackedPropertyError *err) -> bool { return s_vrInstance->WillDriftInYaw(err); });
}
int Lua::openvr::lib::device_is_wireless(lua_State *l)
{
	return get_property_bool(l, [](vr::TrackedPropertyError *err) -> bool { return s_vrInstance->DeviceIsWireless(err); });
}
int Lua::openvr::lib::device_is_charging(lua_State *l)
{
	return get_property_bool(l, [](vr::TrackedPropertyError *err) -> bool { return s_vrInstance->DeviceIsCharging(err); });
}
int Lua::openvr::lib::firmware_update_available(lua_State *l)
{
	return get_property_bool(l, [](vr::TrackedPropertyError *err) -> bool { return s_vrInstance->FirmwareUpdateAvailable(err); });
}
int Lua::openvr::lib::firmware_manual_update(lua_State *l)
{
	return get_property_bool(l, [](vr::TrackedPropertyError *err) -> bool { return s_vrInstance->FirmwareManualUpdate(err); });
}
int Lua::openvr::lib::block_server_shutdown(lua_State *l)
{
	return get_property_bool(l, [](vr::TrackedPropertyError *err) -> bool { return s_vrInstance->BlockServerShutdown(err); });
}
int Lua::openvr::lib::can_unify_coordinate_system_with_hmd(lua_State *l)
{
	return get_property_bool(l, [](vr::TrackedPropertyError *err) -> bool { return s_vrInstance->CanUnifyCoordinateSystemWithHmd(err); });
}
int Lua::openvr::lib::contains_proximity_sensor(lua_State *l)
{
	return get_property_bool(l, [](vr::TrackedPropertyError *err) -> bool { return s_vrInstance->ContainsProximitySensor(err); });
}
int Lua::openvr::lib::device_provides_battery_status(lua_State *l)
{
	return get_property_bool(l, [](vr::TrackedPropertyError *err) -> bool { return s_vrInstance->DeviceProvidesBatteryStatus(err); });
}
int Lua::openvr::lib::device_can_power_off(lua_State *l)
{
	return get_property_bool(l, [](vr::TrackedPropertyError *err) -> bool { return s_vrInstance->DeviceCanPowerOff(err); });
}
int Lua::openvr::lib::has_camera(lua_State *l)
{
	return get_property_bool(l, [](vr::TrackedPropertyError *err) -> bool { return s_vrInstance->HasCamera(err); });
}
int Lua::openvr::lib::reports_time_since_vsync(lua_State *l)
{
	return get_property_bool(l, [](vr::TrackedPropertyError *err) -> bool { return s_vrInstance->ReportsTimeSinceVSync(err); });
}
int Lua::openvr::lib::is_on_desktop(lua_State *l)
{
	return get_property_bool(l, [](vr::TrackedPropertyError *err) -> bool { return s_vrInstance->IsOnDesktop(err); });
}

int Lua::openvr::lib::get_device_battery_percentage(lua_State *l)
{
	return get_property_float(l, [](vr::TrackedPropertyError *err) -> float { return s_vrInstance->GetDeviceBatteryPercentage(err); });
}
int Lua::openvr::lib::get_seconds_from_vsync_to_photons(lua_State *l)
{
	return get_property_float(l, [](vr::TrackedPropertyError *err) -> float { return s_vrInstance->GetSecondsFromVsyncToPhotons(err); });
}
int Lua::openvr::lib::get_display_frequency(lua_State *l)
{
	return get_property_float(l, [](vr::TrackedPropertyError *err) -> float { return s_vrInstance->GetDisplayFrequency(err); });
}
int Lua::openvr::lib::get_user_ipd_meters(lua_State *l)
{
	return get_property_float(l, [](vr::TrackedPropertyError *err) -> float { return s_vrInstance->GetUserIpdMeters(err); });
}
int Lua::openvr::lib::get_display_mc_offset(lua_State *l)
{
	return get_property_float(l, [](vr::TrackedPropertyError *err) -> float { return s_vrInstance->GetDisplayMCOffset(err); });
}
int Lua::openvr::lib::get_display_mc_scale(lua_State *l)
{
	return get_property_float(l, [](vr::TrackedPropertyError *err) -> float { return s_vrInstance->GetDisplayMCScale(err); });
}
int Lua::openvr::lib::get_display_gc_black_clamp(lua_State *l)
{
	return get_property_float(l, [](vr::TrackedPropertyError *err) -> float { return s_vrInstance->GetDisplayGCBlackClamp(err); });
}
int Lua::openvr::lib::get_display_gc_offset(lua_State *l)
{
	return get_property_float(l, [](vr::TrackedPropertyError *err) -> float { return s_vrInstance->GetDisplayGCOffset(err); });
}
int Lua::openvr::lib::get_display_gc_scale(lua_State *l)
{
	return get_property_float(l, [](vr::TrackedPropertyError *err) -> float { return s_vrInstance->GetDisplayGCScale(err); });
}
int Lua::openvr::lib::get_display_gc_prescale(lua_State *l)
{
	return get_property_float(l, [](vr::TrackedPropertyError *err) -> float { return s_vrInstance->GetDisplayGCPrescale(err); });
}
int Lua::openvr::lib::get_lens_center_left_u(lua_State *l)
{
	return get_property_float(l, [](vr::TrackedPropertyError *err) -> float { return s_vrInstance->GetLensCenterLeftU(err); });
}
int Lua::openvr::lib::get_lens_center_left_v(lua_State *l)
{
	return get_property_float(l, [](vr::TrackedPropertyError *err) -> float { return s_vrInstance->GetLensCenterLeftV(err); });
}
int Lua::openvr::lib::get_lens_center_left_uv(lua_State *l)
{
	return get_property_vec2(l, [](vr::TrackedPropertyError *err) -> glm::vec2 { return s_vrInstance->GetLensCenterLeftUV(err); });
}
int Lua::openvr::lib::get_lens_center_right_u(lua_State *l)
{
	return get_property_float(l, [](vr::TrackedPropertyError *err) -> float { return s_vrInstance->GetLensCenterRightU(err); });
}
int Lua::openvr::lib::get_lens_center_right_v(lua_State *l)
{
	return get_property_float(l, [](vr::TrackedPropertyError *err) -> float { return s_vrInstance->GetLensCenterRightV(err); });
}
int Lua::openvr::lib::get_lens_center_right_uv(lua_State *l)
{
	return get_property_vec2(l, [](vr::TrackedPropertyError *err) -> glm::vec2 { return s_vrInstance->GetLensCenterRightUV(err); });
}
int Lua::openvr::lib::get_user_head_to_eye_depth_meters(lua_State *l)
{
	return get_property_float(l, [](vr::TrackedPropertyError *err) -> float { return s_vrInstance->GetUserHeadToEyeDepthMeters(err); });
}
int Lua::openvr::lib::get_field_of_view_left_degrees(lua_State *l)
{
	return get_property_float(l, [](vr::TrackedPropertyError *err) -> float { return s_vrInstance->GetFieldOfViewLeftDegrees(err); });
}
int Lua::openvr::lib::get_field_of_view_right_degrees(lua_State *l)
{
	return get_property_float(l, [](vr::TrackedPropertyError *err) -> float { return s_vrInstance->GetFieldOfViewRightDegrees(err); });
}
int Lua::openvr::lib::get_field_of_view_top_degrees(lua_State *l)
{
	return get_property_float(l, [](vr::TrackedPropertyError *err) -> float { return s_vrInstance->GetFieldOfViewTopDegrees(err); });
}
int Lua::openvr::lib::get_field_of_view_bottom_degrees(lua_State *l)
{
	return get_property_float(l, [](vr::TrackedPropertyError *err) -> float { return s_vrInstance->GetFieldOfViewBottomDegrees(err); });
}
int Lua::openvr::lib::get_tracking_range_minimum_meters(lua_State *l)
{
	return get_property_float(l, [](vr::TrackedPropertyError *err) -> float { return s_vrInstance->GetTrackingRangeMinimumMeters(err); });
}
int Lua::openvr::lib::get_tracking_range_maximum_meters(lua_State *l)
{
	return get_property_float(l, [](vr::TrackedPropertyError *err) -> float { return s_vrInstance->GetTrackingRangeMaximumMeters(err); });
}

int Lua::openvr::lib::get_status_display_transform(lua_State *l)
{
	return get_property_mat3x4(l, [](vr::TrackedPropertyError *err) -> glm::mat3x4 { return s_vrInstance->GetStatusDisplayTransform(err); });
}
int Lua::openvr::lib::get_camera_to_head_transform(lua_State *l)
{
	return get_property_mat3x4(l, [](vr::TrackedPropertyError *err) -> glm::mat3x4 { return s_vrInstance->GetCameraToHeadTransform(err); });
}

int Lua::openvr::lib::get_hardware_revision_number(lua_State *l)
{
	return get_property_uint64(l, [](vr::TrackedPropertyError *err) -> uint64_t { return s_vrInstance->GetHardwareRevisionNumber(err); });
}
int Lua::openvr::lib::get_firmware_version(lua_State *l)
{
	return get_property_uint64(l, [](vr::TrackedPropertyError *err) -> uint64_t { return s_vrInstance->GetFirmwareVersion(err); });
}
int Lua::openvr::lib::get_fpga_version(lua_State *l)
{
	return get_property_uint64(l, [](vr::TrackedPropertyError *err) -> uint64_t { return s_vrInstance->GetFPGAVersion(err); });
}
int Lua::openvr::lib::get_vrc_version(lua_State *l)
{
	return get_property_uint64(l, [](vr::TrackedPropertyError *err) -> uint64_t { return s_vrInstance->GetVRCVersion(err); });
}
int Lua::openvr::lib::get_radio_version(lua_State *l)
{
	return get_property_uint64(l, [](vr::TrackedPropertyError *err) -> uint64_t { return s_vrInstance->GetRadioVersion(err); });
}
int Lua::openvr::lib::get_dongle_version(lua_State *l)
{
	return get_property_uint64(l, [](vr::TrackedPropertyError *err) -> uint64_t { return s_vrInstance->GetDongleVersion(err); });
}
int Lua::openvr::lib::get_current_universe_id(lua_State *l)
{
	return get_property_uint64(l, [](vr::TrackedPropertyError *err) -> uint64_t { return s_vrInstance->GetCurrentUniverseId(err); });
}
int Lua::openvr::lib::get_previous_universe_id(lua_State *l)
{
	return get_property_uint64(l, [](vr::TrackedPropertyError *err) -> uint64_t { return s_vrInstance->GetPreviousUniverseId(err); });
}
int Lua::openvr::lib::get_display_firmware_version(lua_State *l)
{
	return get_property_uint64(l, [](vr::TrackedPropertyError *err) -> uint64_t { return s_vrInstance->GetDisplayFirmwareVersion(err); });
}
int Lua::openvr::lib::get_camera_firmware_version(lua_State *l)
{
	return get_property_uint64(l, [](vr::TrackedPropertyError *err) -> uint64_t { return s_vrInstance->GetCameraFirmwareVersion(err); });
}
int Lua::openvr::lib::get_display_fpga_version(lua_State *l)
{
	return get_property_uint64(l, [](vr::TrackedPropertyError *err) -> uint64_t { return s_vrInstance->GetDisplayFPGAVersion(err); });
}
int Lua::openvr::lib::get_display_bootloader_version(lua_State *l)
{
	return get_property_uint64(l, [](vr::TrackedPropertyError *err) -> uint64_t { return s_vrInstance->GetDisplayBootloaderVersion(err); });
}
int Lua::openvr::lib::get_display_hardware_version(lua_State *l)
{
	return get_property_uint64(l, [](vr::TrackedPropertyError *err) -> uint64_t { return s_vrInstance->GetDisplayHardwareVersion(err); });
}
int Lua::openvr::lib::get_audio_firmware_version(lua_State *l)
{
	return get_property_uint64(l, [](vr::TrackedPropertyError *err) -> uint64_t { return s_vrInstance->GetAudioFirmwareVersion(err); });
}
int Lua::openvr::lib::get_supported_buttons(lua_State *l)
{
	return get_property_uint64(l, [](vr::TrackedPropertyError *err) -> uint64_t { return s_vrInstance->GetSupportedButtons(err); });
}

int Lua::openvr::lib::get_device_class(lua_State *l)
{
	return get_property_int32(l, [](vr::TrackedPropertyError *err) -> int32_t { return s_vrInstance->GetDeviceClass(err); });
}
int Lua::openvr::lib::get_display_mc_type(lua_State *l)
{
	return get_property_int32(l, [](vr::TrackedPropertyError *err) -> int32_t { return s_vrInstance->GetDisplayMCType(err); });
}
int Lua::openvr::lib::get_edid_vendor_id(lua_State *l)
{
	return get_property_int32(l, [](vr::TrackedPropertyError *err) -> int32_t { return s_vrInstance->GetEdidVendorID(err); });
}
int Lua::openvr::lib::get_edid_product_id(lua_State *l)
{
	return get_property_int32(l, [](vr::TrackedPropertyError *err) -> int32_t { return s_vrInstance->GetEdidProductID(err); });
}
int Lua::openvr::lib::get_display_gc_type(lua_State *l)
{
	return get_property_int32(l, [](vr::TrackedPropertyError *err) -> int32_t { return s_vrInstance->GetDisplayGCType(err); });
}
int Lua::openvr::lib::get_camera_compatibility_mode(lua_State *l)
{
	return get_property_int32(l, [](vr::TrackedPropertyError *err) -> int32_t { return s_vrInstance->GetCameraCompatibilityMode(err); });
}
int Lua::openvr::lib::get_axis0_type(lua_State *l)
{
	return get_property_int32(l, [](vr::TrackedPropertyError *err) -> int32_t { return s_vrInstance->GetAxis0Type(err); });
}
int Lua::openvr::lib::get_axis1_type(lua_State *l)
{
	return get_property_int32(l, [](vr::TrackedPropertyError *err) -> int32_t { return s_vrInstance->GetAxis1Type(err); });
}
int Lua::openvr::lib::get_axis2_type(lua_State *l)
{
	return get_property_int32(l, [](vr::TrackedPropertyError *err) -> int32_t { return s_vrInstance->GetAxis2Type(err); });
}
int Lua::openvr::lib::get_axis3_type(lua_State *l)
{
	return get_property_int32(l, [](vr::TrackedPropertyError *err) -> int32_t { return s_vrInstance->GetAxis3Type(err); });
}
int Lua::openvr::lib::get_axis4_type(lua_State *l)
{
	return get_property_int32(l, [](vr::TrackedPropertyError *err) -> int32_t { return s_vrInstance->GetAxis4Type(err); });
}

int Lua::openvr::lib::fade_to_color(lua_State *l)
{
	if(s_vrInstance == nullptr)
		return 0;
	auto *col = Lua::CheckColor(l, 1);
	auto tFade = Lua::CheckNumber(l, 2);
	auto bBackground = false;
	if(Lua::IsSet(l, 3))
		bBackground = Lua::CheckBool(l, 3);
	s_vrInstance->FadeToColor(*col, tFade, bBackground);
	return 0;
}
int Lua::openvr::lib::fade_grid(lua_State *l)
{
	if(s_vrInstance == nullptr)
		return 0;
	auto tFade = Lua::CheckNumber(l, 1);
	auto bFadeIn = Lua::CheckNumber(l, 2);
	s_vrInstance->FadeGrid(tFade, bFadeIn);
	return 0;
}
int Lua::openvr::lib::show_mirror_window(lua_State *l)
{
	if(s_vrInstance == nullptr)
		return 0;
	s_vrInstance->ShowMirrorWindow();
	return 0;
}
int Lua::openvr::lib::hide_mirror_window(lua_State *l)
{
	if(s_vrInstance == nullptr)
		return 0;
	s_vrInstance->HideMirrorWindow();
	return 0;
}
int Lua::openvr::lib::is_mirror_window_visible(lua_State *l)
{
	if(s_vrInstance == nullptr)
		Lua::PushBool(l, false);
	else
		Lua::PushBool(l, s_vrInstance->IsMirrorWindowVisible());
	return 1;
}
int Lua::openvr::lib::set_hmd_view_enabled(lua_State *l)
{
	if(s_vrInstance == nullptr)
		return 0;
	auto b = Lua::CheckBool(l, 1);
	s_vrInstance->SetHmdViewEnabled(b);
	return 0;
}
int Lua::openvr::lib::is_hmd_view_enabled(lua_State *l)
{
	if(s_vrInstance == nullptr)
		Lua::PushBool(l, false);
	else
		Lua::PushBool(l, s_vrInstance->IsHmdViewEnabled());
	return 1;
}
int Lua::openvr::lib::can_render_scene(lua_State *l)
{
	if(s_vrInstance == nullptr)
		Lua::PushBool(l, false);
	else
		Lua::PushBool(l, s_vrInstance->CanRenderScene());
	return 1;
}
int Lua::openvr::lib::clear_last_submitted_frame(lua_State *l)
{
	if(s_vrInstance == nullptr)
		return 0;
	s_vrInstance->ClearLastSubmittedFrame();
	return 0;
}
int Lua::openvr::lib::clear_skybox_override(lua_State *l)
{
	if(s_vrInstance == nullptr)
		return 0;
	s_vrInstance->ClearSkyboxOverride();
	return 0;
}
int Lua::openvr::lib::compositor_bring_to_front(lua_State *l)
{
	if(s_vrInstance == nullptr)
		return 0;
	s_vrInstance->CompositorBringToFront();
	return 0;
}
int Lua::openvr::lib::compositor_dump_images(lua_State *l)
{
	if(s_vrInstance == nullptr)
		return 0;
	s_vrInstance->CompositorDumpImages();
	return 0;
}
int Lua::openvr::lib::compositor_go_to_back(lua_State *l)
{
	if(s_vrInstance == nullptr)
		return 0;
	s_vrInstance->CompositorGoToBack();
	return 0;
}
int Lua::openvr::lib::force_interleaved_reprojection_on(lua_State *l)
{
	if(s_vrInstance == nullptr)
		return 0;
	auto b = Lua::CheckBool(l, 1);
	s_vrInstance->ForceInterleavedReprojectionOn(b);
	return 0;
}
int Lua::openvr::lib::force_reconnect_process(lua_State *l)
{
	if(s_vrInstance == nullptr)
		return 0;
	s_vrInstance->ForceReconnectProcess();
	return 0;
}
int Lua::openvr::lib::get_frame_time_remaining(lua_State *l)
{
	if(s_vrInstance == nullptr)
		Lua::PushNumber(l, 0.f);
	else
		Lua::PushNumber(l, s_vrInstance->GetFrameTimeRemaining());
	return 1;
}
int Lua::openvr::lib::is_fullscreen(lua_State *l)
{
	if(s_vrInstance == nullptr)
		Lua::PushBool(l, false);
	else
		Lua::PushBool(l, s_vrInstance->IsFullscreen());
	return 1;
}
int Lua::openvr::lib::should_app_render_with_low_resources(lua_State *l)
{
	if(s_vrInstance == nullptr)
		Lua::PushBool(l, false);
	else
		Lua::PushBool(l, s_vrInstance->ShouldAppRenderWithLowResources());
	return 1;
}
int Lua::openvr::lib::suspend_rendering(lua_State *l)
{
	if(s_vrInstance == nullptr)
		return 0;
	auto b = Lua::CheckBool(l, 1);
	s_vrInstance->SuspendRendering(b);
	return 0;
}
int Lua::openvr::lib::set_skybox_override(lua_State *l)
{
	if(s_vrInstance == nullptr)
		return 0;
	int32_t arg = 1;
	auto &img = Lua::Check<Lua::Vulkan::Image>(l, arg++);
	if(Lua::IsSet(l, arg) == false) {
		auto err = s_vrInstance->SetSkyboxOverride(img);
		Lua::PushInt(l, umath::to_integral(err));
		return 1;
	}
	auto &img2 = Lua::Check<Lua::Vulkan::Image>(l, arg++);
	if(Lua::IsSet(l, arg) == false) {
		auto err = s_vrInstance->SetSkyboxOverride(img, img2);
		Lua::PushInt(l, umath::to_integral(err));
		return 1;
	}
	auto &front = img;
	auto &back = img2;
	auto &left = Lua::Check<Lua::Vulkan::Image>(l, arg++);
	auto &right = Lua::Check<Lua::Vulkan::Image>(l, arg++);
	auto &top = Lua::Check<Lua::Vulkan::Image>(l, arg++);
	auto &bottom = Lua::Check<Lua::Vulkan::Image>(l, arg++);
	auto err = s_vrInstance->SetSkyboxOverride(front, back, left, right, top, bottom);
	Lua::PushInt(l, umath::to_integral(err));
	return 1;
}
int Lua::openvr::lib::get_cumulative_stats(lua_State *l)
{
	if(s_vrInstance == nullptr)
		return 0;
	auto stats = s_vrInstance->GetCumulativeStats();
	auto t = Lua::CreateTable(l);

	const auto fAddAttribute = [l, t, &stats](const std::string id, uint32_t val) {
		Lua::PushString(l, id);
		Lua::PushInt(l, val);
		Lua::SetTableValue(l, t);
	};
	fAddAttribute("numFramePresents", stats.m_nNumFramePresents);
	fAddAttribute("numDroppedFrames", stats.m_nNumDroppedFrames);
	fAddAttribute("numReprojectedFrames", stats.m_nNumReprojectedFrames);
	fAddAttribute("numFramePresentsOnStartup", stats.m_nNumFramePresentsOnStartup);
	fAddAttribute("numDroppedFramesOnStartup", stats.m_nNumDroppedFramesOnStartup);
	fAddAttribute("numReprojectedFramesOnStartup", stats.m_nNumReprojectedFramesOnStartup);
	fAddAttribute("numLoading", stats.m_nNumLoading);
	fAddAttribute("numFramePresentsLoading", stats.m_nNumFramePresentsLoading);
	fAddAttribute("numDroppedFramesLoading", stats.m_nNumDroppedFramesLoading);
	fAddAttribute("numReprojectedFramesLoading", stats.m_nNumReprojectedFramesLoading);
	fAddAttribute("numTimedOut", stats.m_nNumTimedOut);
	fAddAttribute("numFramePresentsTimedOut", stats.m_nNumFramePresentsTimedOut);
	fAddAttribute("numDroppedFramesTimedOut", stats.m_nNumDroppedFramesTimedOut);
	fAddAttribute("numReprojectedFramesTimedOut", stats.m_nNumReprojectedFramesTimedOut);
	return 1;
}
int Lua::openvr::lib::get_tracking_space(lua_State *l)
{
	if(s_vrInstance == nullptr)
		return 0;
	auto space = s_vrInstance->GetTrackingSpace();
	Lua::PushInt(l, umath::to_integral(space));
	return 1;
}
int Lua::openvr::lib::set_tracking_space(lua_State *l)
{
	if(s_vrInstance == nullptr)
		return 0;
	auto space = Lua::CheckInt(l, 1);
	s_vrInstance->SetTrackingSpace(static_cast<vr::ETrackingUniverseOrigin>(space));
	return 0;
}

int Lua::openvr::lib::get_recommended_render_target_size(lua_State *l)
{
	if(s_vrInstance == nullptr) {
		// Standard Vive resolution (per eye)
		Lua::PushInt(l, 1'080);
		Lua::PushInt(l, 1'200);
	}
	else {
		auto *sys = s_vrInstance->GetSystemInterface();
		auto width = 0u;
		auto height = 0u;
		sys->GetRecommendedRenderTargetSize(&width, &height);
		Lua::PushInt(l, width);
		Lua::PushInt(l, height);
	}
	return 2;
}

int Lua::openvr::lib::get_projection_matrix(lua_State *l)
{
	if(s_vrInstance == nullptr)
		Lua::Push<Mat4>(l, {});
	else {
		auto eye = Lua::CheckInt(l, 1);
		auto nearZ = Lua::CheckNumber(l, 2);
		auto farZ = Lua::CheckNumber(l, 3);
		auto *sys = s_vrInstance->GetSystemInterface();
		auto vrMat = sys->GetProjectionMatrix(static_cast<vr::EVREye>(eye), nearZ, farZ);
		auto m = glm::transpose(reinterpret_cast<Mat4 &>(vrMat.m));
		m = glm::scale(m, Vector3(1.f, -1.f, 1.f));
		Lua::Push<Mat4>(l, m);
	}
	return 1;
}

int Lua::openvr::lib::get_projection_raw(lua_State *l)
{
	auto left = 0.f;
	auto right = 0.f;
	auto top = 0.f;
	auto bottom = 0.f;
	if(s_vrInstance != nullptr) {
		auto *sys = s_vrInstance->GetSystemInterface();
		auto eye = Lua::CheckInt(l, 1);
		sys->GetProjectionRaw(static_cast<vr::EVREye>(eye), &left, &right, &top, &bottom);
	}
	Lua::PushNumber(l, left);
	Lua::PushNumber(l, right);
	Lua::PushNumber(l, top);
	Lua::PushNumber(l, bottom);
	return 4;
}

int Lua::openvr::lib::compute_distortion(lua_State *l)
{
	if(s_vrInstance == nullptr)
		return 0;
	auto *sys = s_vrInstance->GetSystemInterface();
	auto eye = Lua::CheckInt(l, 1);
	auto fu = Lua::CheckNumber(l, 2);
	auto fv = Lua::CheckNumber(l, 3);
	vr::DistortionCoordinates_t distortion {};
	auto b = sys->ComputeDistortion(static_cast<vr::EVREye>(eye), fu, fv, &distortion);
	Lua::PushBool(l, b);
	if(b == true) {
		Lua::Push<Vector2>(l, reinterpret_cast<Vector2 &>(distortion.rfRed));
		Lua::Push<Vector2>(l, reinterpret_cast<Vector2 &>(distortion.rfGreen));
		Lua::Push<Vector2>(l, reinterpret_cast<Vector2 &>(distortion.rfBlue));
		return 4;
	}
	return 1;
}

int Lua::openvr::lib::get_eye_to_head_transform(lua_State *l)
{
	if(s_vrInstance == nullptr)
		return 0;
	auto *sys = s_vrInstance->GetSystemInterface();
	auto eEye = static_cast<vr::EVREye>(Lua::CheckInt(l, 1));
	auto *cam = luabind::object_cast_nothrow<pragma::CCameraComponent *>(luabind::object {luabind::from_stack(l, 2)}, static_cast<pragma::CCameraComponent *>(nullptr));
	if(!cam)
		return 0;
	auto &eye = (eEye == vr::EVREye::Eye_Left) ? s_vrInstance->GetLeftEye() : s_vrInstance->GetRightEye();
	Lua::Push<Mat4>(l, eye.GetEyeViewMatrix(*cam));
	return 1;
}

int Lua::openvr::lib::get_time_since_last_vsync(lua_State *l)
{
	if(s_vrInstance == nullptr)
		return 0;
	auto *sys = s_vrInstance->GetSystemInterface();
	float secondsSinceLasyVsync = 0.f;
	uint64_t pullFrameCounter = 0ull;
	auto r = sys->GetTimeSinceLastVsync(&secondsSinceLasyVsync, &pullFrameCounter);
	Lua::PushBool(l, r);
	Lua::PushNumber(l, secondsSinceLasyVsync);
	Lua::PushInt(l, pullFrameCounter);
	return 3;
}

int Lua::openvr::lib::get_device_to_absolute_tracking_pose(lua_State *l)
{
	if(s_vrInstance == nullptr)
		return 0;
	auto *sys = s_vrInstance->GetSystemInterface();
	auto origin = Lua::CheckInt(l, 1);
	auto predictedSecondsToPhotonsFromNow = Lua::CheckNumber(l, 2);
	static std::vector<vr::TrackedDevicePose_t> trackedDevicePoseArray(vr::k_unMaxTrackedDeviceCount);
	sys->GetDeviceToAbsoluteTrackingPose(static_cast<vr::ETrackingUniverseOrigin>(origin), predictedSecondsToPhotonsFromNow, trackedDevicePoseArray.data(), trackedDevicePoseArray.size());

	auto t = Lua::CreateTable(l);
	int32_t idx = 1;
	for(auto &tdp : trackedDevicePoseArray) {
		Lua::PushInt(l, idx++);
		Lua::Push<vr::TrackedDevicePose_t>(l, tdp);
		Lua::SetTableValue(l, t);

		Lua::Pop(l, 1);
	}
	return 1;
}

int Lua::openvr::lib::compute_seconds_to_photons(lua_State *l)
{
	if(s_vrInstance == nullptr)
		return 0;
	auto *sys = s_vrInstance->GetSystemInterface();
	float fSecondsSinceLastVsync;
	sys->GetTimeSinceLastVsync(&fSecondsSinceLastVsync, nullptr);

	auto fDisplayFrequency = sys->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_DisplayFrequency_Float);
	auto fFrameDuration = 1.f / fDisplayFrequency;
	auto fVsyncToPhotons = sys->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_SecondsFromVsyncToPhotons_Float);

	auto fPredictedSecondsFromNow = fFrameDuration - fSecondsSinceLastVsync + fVsyncToPhotons;
	Lua::PushNumber(l, fPredictedSecondsFromNow);
	return 1;
}

int Lua::openvr::lib::get_seated_zero_pose_to_standing_absolute_tracking_pose(lua_State *l)
{
	if(s_vrInstance == nullptr)
		return 0;
	auto *sys = s_vrInstance->GetSystemInterface();
	auto m = sys->GetSeatedZeroPoseToStandingAbsoluteTrackingPose();
	Lua::Push<Mat3x4>(l, reinterpret_cast<Mat3x4 &>(m));
	return 1;
}

int Lua::openvr::lib::get_tracked_device_class(lua_State *l)
{
	if(s_vrInstance == nullptr)
		return 0;
	auto *sys = s_vrInstance->GetSystemInterface();
	auto devIndex = Lua::CheckInt(l, 1);
	auto deviceClass = sys->GetTrackedDeviceClass(devIndex);
	Lua::PushInt(l, static_cast<int32_t>(deviceClass));
	return 1;
}

int Lua::openvr::lib::is_tracked_device_connected(lua_State *l)
{
	if(s_vrInstance == nullptr)
		Lua::PushBool(l, false);
	else {
		auto *sys = s_vrInstance->GetSystemInterface();
		auto devIndex = Lua::CheckInt(l, 1);
		auto b = sys->IsTrackedDeviceConnected(devIndex);
		Lua::PushBool(l, b);
	}
	return 1;
}

int Lua::openvr::lib::trigger_haptic_pulse(lua_State *l)
{
	if(s_vrInstance == nullptr)
		return 0;
	auto *sys = s_vrInstance->GetSystemInterface();
	auto devIndex = Lua::CheckInt(l, 1);
	auto axisId = Lua::CheckInt(l, 2);
	auto duration = Lua::CheckNumber(l, 3);
	sys->TriggerHapticPulse(devIndex, axisId, duration * 1'000'000);
	return 0;
}

struct LuaVRControllerState {
	// If packet num matches that on your prior call, then the controller state hasn't been changed since
	// your last call and there is no need to process it
	uint32_t unPacketNum;

	// bit flags for each of the buttons. Use ButtonMaskFromId to turn an ID into a mask
	uint32_t ulButtonPressed;
	uint32_t ulButtonTouched;

	// Axis data for the controller's analog inputs
	Vector2 rAxis0;
	Vector2 rAxis1;
	Vector2 rAxis2;
	Vector2 rAxis3;
	Vector2 rAxis4;
};

static LuaVRControllerState vr_controller_state_to_lua_controller_state(const vr::VRControllerState_t &in)
{
	auto r = LuaVRControllerState {};
	r.unPacketNum = in.unPacketNum;
	r.ulButtonPressed = in.ulButtonPressed;
	r.ulButtonTouched = in.ulButtonTouched;
	r.rAxis0 = {in.rAxis[0].x, in.rAxis[0].y};
	r.rAxis1 = {in.rAxis[1].x, in.rAxis[1].y};
	r.rAxis2 = {in.rAxis[2].x, in.rAxis[2].y};
	r.rAxis3 = {in.rAxis[3].x, in.rAxis[3].y};
	r.rAxis4 = {in.rAxis[4].x, in.rAxis[4].y};
	return r;
}

int Lua::openvr::lib::get_controller_states(lua_State *l)
{
	if(s_vrInstance == nullptr)
		return 0;
	auto *sys = s_vrInstance->GetSystemInterface();
	auto t = Lua::CreateTable(l);

	vr::VRControllerState_t state {};
	for(auto i = decltype(vr::k_unMaxTrackedDeviceCount) {0}; i < vr::k_unMaxTrackedDeviceCount; ++i) {
		vr::VRControllerState_t state;
		if(sys->GetControllerState(i, &state, sizeof(vr::VRControllerState_t))) {
			Lua::PushInt(l, i);
			Lua::Push<LuaVRControllerState>(l, vr_controller_state_to_lua_controller_state(state));
			Lua::SetTableValue(l, t);
		}
	}
	return 1;
}

int Lua::openvr::lib::get_controller_state(lua_State *l)
{
	if(s_vrInstance == nullptr)
		return 0;
	auto *sys = s_vrInstance->GetSystemInterface();
	auto devIndex = Lua::CheckInt(l, 1);
	vr::VRControllerState_t state {};
	auto r = sys->GetControllerState(devIndex, &state, sizeof(vr::VRControllerState_t));
	Lua::PushBool(l, r);
	if(r == true) {
		Lua::Push<LuaVRControllerState>(l, vr_controller_state_to_lua_controller_state(state));
		return 2;
	}
	return 1;
}

int Lua::openvr::lib::get_controller_role(lua_State *l)
{
	if(s_vrInstance == nullptr)
		return 0;
	auto devIndex = Lua::CheckInt(l, 1);
	auto role = s_vrInstance->GetTrackedDeviceRole(devIndex);
	Lua::PushInt(l, umath::to_integral(role));
	return 1;
}

int Lua::openvr::lib::get_controller_state_with_pose(lua_State *l)
{
	if(s_vrInstance == nullptr)
		return 0;
	auto *sys = s_vrInstance->GetSystemInterface();
	auto origin = Lua::CheckInt(l, 1);
	auto devIndex = Lua::CheckInt(l, 2);
	vr::VRControllerState_t state {};
	vr::TrackedDevicePose_t devPose {};
	auto r = sys->GetControllerStateWithPose(static_cast<vr::TrackingUniverseOrigin>(origin), devIndex, &state, sizeof(vr::VRControllerState_t), &devPose);
	Lua::PushBool(l, r);
	if(r == true) {
		Lua::Push<LuaVRControllerState>(l, vr_controller_state_to_lua_controller_state(state));
		Lua::Push<vr::TrackedDevicePose_t>(l, devPose);
		return 2;
	}
	return 1;
}

int Lua::openvr::lib::get_pose_transform(lua_State *l)
{
	if(s_vrInstance == nullptr)
		return 0;
	auto deviceIdx = Lua::CheckInt(l, 1);
	vr::TrackedDevicePose_t pose {};
	Mat4 m {};
	if(s_vrInstance->GetPoseTransform(deviceIdx, pose, m) == false)
		return 0;
	Lua::Push<Mat4>(l, m);
	Lua::Push<Vector3>(l, Vector3(pose.vVelocity.v[0], pose.vVelocity.v[1], pose.vVelocity.v[2]) * static_cast<float>(::util::pragma::metres_to_units(1.f)));
	return 2;
}

static umath::Transform openvr_matrix_to_pragma_pose(const Mat4 &poseMatrix)
{
	Vector3 scale;
	Vector3 skew;
	::Vector4 perspective;
	Vector3 pos;
	Quat rot;
	glm::decompose(poseMatrix, scale, rot, pos, skew, perspective);
	rot = glm::conjugate(rot);

	static auto openVrToPragmaPoseTransform = uquat::create(EulerAngles(0.f, 180.f, 0.f));
	rot = rot * openVrToPragmaPoseTransform;
	pos *= static_cast<float>(::util::pragma::metres_to_units(1.f));
	return umath::Transform {pos, rot};
}

int Lua::openvr::lib::get_pose(lua_State *l)
{
	if(s_vrInstance == nullptr)
		return 0;
	auto deviceIdx = Lua::CheckInt(l, 1);
	vr::TrackedDevicePose_t pose {};
	Mat4 m {};
	//if(s_vrInstance->GetPoseTransform(deviceIdx,pose,m) == false)
	//	return 0;
	m = s_vrInstance->GetPoseMatrix(deviceIdx);

	m = glm::inverse(m);
	auto mpose = openvr_matrix_to_pragma_pose(m);

#if 0
	auto o = mpose.GetOrigin();
	auto pos0 = Vector3{m[3][0],m[3][1],m[3][2]};
	std::cout<<"Test:"<<std::endl;
	std::cout<<o.x<<","<<o.y<<","<<o.z<<std::endl;
	std::cout<<pos0.x<<","<<pos0.y<<","<<pos0.z<<std::endl;
#endif

	// For some reason the position from GetPoseMatrix (which comes from WaitGetPoses)
	// is incorrect, but the rotation is correct, while for GetPoseTransform it's the other way around.
	// We're probably doing something wrong somewhere, but for now this will do as a work-around.
	// TODO: FIXME
	vr::TrackedDevicePose_t pose2 {};
	Mat4 m2 {};
	if(s_vrInstance->GetPoseTransform(deviceIdx, pose2, m2)) {
		auto &pos = mpose.GetOrigin();
		pos = {m2[3][0], m2[3][1], m2[3][2]};
		pos *= static_cast<float>(::util::pragma::metres_to_units(1.f));
	}

	static auto applyRotCorrection = true;
	if(applyRotCorrection) {
		static auto correctionRotation = uquat::create(EulerAngles {0.f, 180.f, 0.f});
		mpose.RotateGlobal(correctionRotation);
	}

	Lua::Push<umath::Transform>(l, mpose);
	Lua::Push<Vector3>(l, Vector3(pose.vVelocity.v[0], pose.vVelocity.v[1], pose.vVelocity.v[2]) * static_cast<float>(::util::pragma::metres_to_units(1.f)));
	return 2;
}

static void add_event_data(const vr::VREvent_t &ev, luabind::object &t)
{
	switch(ev.eventType) {
	case vr::VREvent_ButtonPress:
	case vr::VREvent_ButtonUnpress:
	case vr::VREvent_ButtonTouch:
	case vr::VREvent_ButtonUntouch:
		{
			t["button"] = ev.data.controller.button;
			break;
		}
	case vr::VREvent_MouseMove:
	case vr::VREvent_MouseButtonDown:
	case vr::VREvent_MouseButtonUp:
	case vr::VREvent_TouchPadMove:
		{
			t["button"] = ev.data.mouse.button;
			t["x"] = ev.data.mouse.x;
			t["y"] = ev.data.mouse.y;
			break;
		}
	case vr::VREvent_InputFocusCaptured:
	case vr::VREvent_InputFocusReleased:
	case vr::VREvent_SceneApplicationChanged:
	case vr::VREvent_InputFocusChanged:
	case vr::VREvent_SceneApplicationUsingWrongGraphicsAdapter:
	case vr::VREvent_ActionBindingReloaded:
	case vr::VREvent_Quit:
	case vr::VREvent_ProcessQuit:
	case vr::VREvent_QuitAcknowledged:
	case vr::VREvent_Monitor_ShowHeadsetView:
	case vr::VREvent_Monitor_HideHeadsetView:
		{
			t["connectionLost"] = ev.data.process.bConnectionLost;
			t["forced"] = ev.data.process.bForced;
			t["oldPid"] = ev.data.process.oldPid;
			t["pid"] = ev.data.process.pid;
			break;
		}
	case vr::VREvent_FocusEnter:
	case vr::VREvent_FocusLeave:
	case vr::VREvent_OverlayFocusChanged:
	case vr::VREvent_DashboardRequested:
		{
			//auto &overlay = ev.data.overlay
			break;
		}
	case vr::VREvent_ScrollDiscrete:
	case vr::VREvent_ScrollSmooth:
		{
			t["viewportScale"] = ev.data.scroll.viewportscale;
			t["xdelta"] = ev.data.scroll.xdelta;
			t["ydelta"] = ev.data.scroll.ydelta;
			break;
		}
	case vr::VREvent_ShowUI:
		{
			t["type"] = ev.data.showUi.eType;
			break;
		}
	case vr::VREvent_ShowDevTools:
		{
			t["browserIdentifier"] = ev.data.showDevTools.nBrowserIdentifier;
			break;
		}
	case vr::VREvent_Compositor_HDCPError:
		{
			t["code"] = ev.data.hdcpError.eCode;
			break;
		}
	case vr::VREvent_Input_HapticVibration:
		{
			t["amplitude"] = ev.data.hapticVibration.fAmplitude;
			t["durationSeconds"] = ev.data.hapticVibration.fDurationSeconds;
			t["frequency"] = ev.data.hapticVibration.fFrequency;
			break;
		}
	case vr::VREvent_Input_BindingLoadFailed:
	case vr::VREvent_Input_BindingLoadSuccessful:
		{
			t["pathControllerType"] = ev.data.inputBinding.pathControllerType;
			break;
		}
	case vr::VREvent_Input_ActionManifestLoadFailed:
		{
			//auto &actionManifest = ev.data.actionManifest
			break;
		}
	case vr::VREvent_Input_ProgressUpdate:
		{
			t["progress"] = ev.data.progressUpdate.fProgress;
			t["pathProgressAction"] = ev.data.progressUpdate.pathProgressAction;
			break;
		}
	case vr::VREvent_SpatialAnchors_PoseUpdated:
		{
			//auto &spatialAnchor = ev.data.spatialAnchor
			break;
		}
	}
}

void Lua::openvr::register_lua_library(Lua::Interface &l)
{
	auto *lua = l.GetState();
	Lua::RegisterLibrary(lua, "openvr",
	  {
	    {"initialize", Lua::openvr::lib::initialize},
	    {"preinitialize",
	      +[](lua_State *l) -> int {
		      ::openvr::preinitialize_openvr();
		      return 0;
	      }},
	    {"is_hmd_present",
	      +[](lua_State *l) -> int {
		      Lua::PushBool(l, ::openvr::is_hmd_present());
		      return 1;
	      }},
	    {"close", Lua::openvr::lib::close},

	    {"property_error_to_string", Lua::openvr::lib::property_error_to_string}, {"init_error_to_string", Lua::openvr::lib::init_error_to_string}, {"compositor_error_to_string", Lua::openvr::lib::compositor_error_to_string}, {"button_id_to_string", Lua::openvr::lib::button_id_to_string},
	    {"controller_axis_type_to_string", Lua::openvr::lib::controller_axis_type_to_string}, {"event_type_to_string", Lua::openvr::lib::event_type_to_string},

	    {"get_tracking_system_name", Lua::openvr::lib::get_tracking_system_name}, {"get_model_number", Lua::openvr::lib::get_model_number}, {"get_serial_number", Lua::openvr::lib::get_serial_number}, {"get_render_model_name", Lua::openvr::lib::get_render_model_name},
	    {"get_manufacturer_name", Lua::openvr::lib::get_manufacturer_name}, {"get_tracking_firmware_version", Lua::openvr::lib::get_tracking_firmware_version}, {"get_hardware_revision", Lua::openvr::lib::get_hardware_revision},
	    {"get_all_wireless_dongle_descriptions", Lua::openvr::lib::get_all_wireless_dongle_descriptions}, {"get_connected_wireless_dongle", Lua::openvr::lib::get_connected_wireless_dongle}, {"get_firmware_manual_update_url", Lua::openvr::lib::get_firmware_manual_update_url},
	    {"get_firmware_programming_target", Lua::openvr::lib::get_firmware_programming_target}, {"get_display_mc_image_left", Lua::openvr::lib::get_display_mc_image_left}, {"get_display_mc_image_right", Lua::openvr::lib::get_display_mc_image_right},
	    {"get_display_gc_image", Lua::openvr::lib::get_display_gc_image}, {"get_camera_firmware_description", Lua::openvr::lib::get_camera_firmware_description}, {"get_attached_device_id", Lua::openvr::lib::get_attached_device_id}, {"get_model_label", Lua::openvr::lib::get_model_label},

	    {"will_drift_in_yaw", Lua::openvr::lib::will_drift_in_yaw}, {"device_is_wireless", Lua::openvr::lib::device_is_wireless}, {"device_is_charging", Lua::openvr::lib::device_is_charging}, {"firmware_update_available", Lua::openvr::lib::firmware_update_available},
	    {"firmware_manual_update", Lua::openvr::lib::firmware_manual_update}, {"block_server_shutdown", Lua::openvr::lib::block_server_shutdown}, {"can_unify_coordinate_system_with_hmd", Lua::openvr::lib::can_unify_coordinate_system_with_hmd},
	    {"contains_proximity_sensor", Lua::openvr::lib::contains_proximity_sensor}, {"device_provides_battery_status", Lua::openvr::lib::device_provides_battery_status}, {"device_can_power_off", Lua::openvr::lib::device_can_power_off}, {"has_camera", Lua::openvr::lib::has_camera},
	    {"reports_time_since_vsync", Lua::openvr::lib::reports_time_since_vsync}, {"is_on_desktop", Lua::openvr::lib::is_on_desktop},

	    {"get_device_battery_percentage", Lua::openvr::lib::get_device_battery_percentage}, {"get_seconds_from_vsync_to_photons", Lua::openvr::lib::get_seconds_from_vsync_to_photons}, {"get_display_frequency", Lua::openvr::lib::get_display_frequency},
	    {"get_user_ipd_meters", Lua::openvr::lib::get_user_ipd_meters}, {"get_display_mc_offset", Lua::openvr::lib::get_display_mc_offset}, {"get_display_mc_scale", Lua::openvr::lib::get_display_mc_scale}, {"get_display_gc_black_clamp", Lua::openvr::lib::get_display_gc_black_clamp},
	    {"get_display_gc_offset", Lua::openvr::lib::get_display_gc_offset}, {"get_display_gc_scale", Lua::openvr::lib::get_display_gc_scale}, {"get_display_gc_prescale", Lua::openvr::lib::get_display_gc_prescale}, {"get_lens_center_left_u", Lua::openvr::lib::get_lens_center_left_u},
	    {"get_lens_center_left_v", Lua::openvr::lib::get_lens_center_left_v}, {"get_lens_center_left_uv", Lua::openvr::lib::get_lens_center_left_uv}, {"get_lens_center_right_u", Lua::openvr::lib::get_lens_center_right_u},
	    {"get_lens_center_right_v", Lua::openvr::lib::get_lens_center_right_v}, {"get_lens_center_right_uv", Lua::openvr::lib::get_lens_center_right_uv}, {"get_user_head_to_eye_depth_meters", Lua::openvr::lib::get_user_head_to_eye_depth_meters},
	    {"get_field_of_view_left_degrees", Lua::openvr::lib::get_field_of_view_left_degrees}, {"get_field_of_view_right_degrees", Lua::openvr::lib::get_field_of_view_right_degrees}, {"get_field_of_view_top_degrees", Lua::openvr::lib::get_field_of_view_top_degrees},
	    {"get_field_of_view_bottom_degrees", Lua::openvr::lib::get_field_of_view_bottom_degrees}, {"get_tracking_range_minimum_meters", Lua::openvr::lib::get_tracking_range_minimum_meters}, {"get_tracking_range_maximum_meters", Lua::openvr::lib::get_tracking_range_maximum_meters},

	    {"get_status_display_transform", Lua::openvr::lib::get_status_display_transform}, {"get_camera_to_head_transform", Lua::openvr::lib::get_camera_to_head_transform},

	    {"get_hardware_revision_number", Lua::openvr::lib::get_hardware_revision_number}, {"get_firmware_version", Lua::openvr::lib::get_firmware_version}, {"get_fpga_version", Lua::openvr::lib::get_fpga_version}, {"get_vrc_version", Lua::openvr::lib::get_vrc_version},
	    {"get_radio_version", Lua::openvr::lib::get_radio_version}, {"get_dongle_version", Lua::openvr::lib::get_dongle_version}, {"get_current_universe_id", Lua::openvr::lib::get_current_universe_id}, {"get_previous_universe_id", Lua::openvr::lib::get_previous_universe_id},
	    {"get_display_firmware_version", Lua::openvr::lib::get_display_firmware_version}, {"get_camera_firmware_version", Lua::openvr::lib::get_camera_firmware_version}, {"get_display_fpga_version", Lua::openvr::lib::get_display_fpga_version},
	    {"get_display_bootloader_version", Lua::openvr::lib::get_display_bootloader_version}, {"get_display_hardware_version", Lua::openvr::lib::get_display_hardware_version}, {"get_audio_firmware_version", Lua::openvr::lib::get_audio_firmware_version},
	    {"get_supported_buttons", Lua::openvr::lib::get_supported_buttons},

	    {"get_device_class", Lua::openvr::lib::get_device_class}, {"get_display_mc_type", Lua::openvr::lib::get_display_mc_type}, {"get_edid_vendor_id", Lua::openvr::lib::get_edid_vendor_id}, {"get_edid_product_id", Lua::openvr::lib::get_edid_product_id},
	    {"get_display_gc_type", Lua::openvr::lib::get_display_gc_type}, {"get_camera_compatibility_mode", Lua::openvr::lib::get_camera_compatibility_mode}, {"get_axis0_type", Lua::openvr::lib::get_axis0_type}, {"get_axis1_type", Lua::openvr::lib::get_axis1_type},
	    {"get_axis2_type", Lua::openvr::lib::get_axis2_type}, {"get_axis3_type", Lua::openvr::lib::get_axis3_type}, {"get_axis4_type", Lua::openvr::lib::get_axis4_type},

	    {"fade_to_color", Lua::openvr::lib::fade_to_color}, {"fade_grid", Lua::openvr::lib::fade_grid}, {"show_mirror_window", Lua::openvr::lib::show_mirror_window}, {"hide_mirror_window", Lua::openvr::lib::hide_mirror_window},
	    {"is_mirror_window_visible", Lua::openvr::lib::is_mirror_window_visible}, {"set_hmd_view_enabled", Lua::openvr::lib::set_hmd_view_enabled}, {"is_hmd_view_enabled", Lua::openvr::lib::is_hmd_view_enabled},

	    {"can_render_scene", Lua::openvr::lib::can_render_scene}, {"clear_last_submitted_frame", Lua::openvr::lib::clear_last_submitted_frame}, {"clear_skybox_override", Lua::openvr::lib::clear_skybox_override}, {"compositor_bring_to_front", Lua::openvr::lib::compositor_bring_to_front},
	    {"compositor_dump_images", Lua::openvr::lib::compositor_dump_images}, {"compositor_go_to_back", Lua::openvr::lib::compositor_go_to_back}, {"force_interleaved_reprojection_on", Lua::openvr::lib::force_interleaved_reprojection_on},
	    {"force_reconnect_process", Lua::openvr::lib::force_reconnect_process}, {"get_frame_time_remaining", Lua::openvr::lib::get_frame_time_remaining}, {"is_fullscreen", Lua::openvr::lib::is_fullscreen},
	    {"should_app_render_with_low_resources", Lua::openvr::lib::should_app_render_with_low_resources}, {"suspend_rendering", Lua::openvr::lib::suspend_rendering}, {"set_skybox_override", Lua::openvr::lib::set_skybox_override},
	    {"get_cumulative_stats", Lua::openvr::lib::get_cumulative_stats}, {"get_tracking_space", Lua::openvr::lib::get_tracking_space}, {"set_tracking_space", Lua::openvr::lib::set_tracking_space},

	    {"get_recommended_render_target_size", Lua::openvr::lib::get_recommended_render_target_size}, {"get_projection_matrix", Lua::openvr::lib::get_projection_matrix}, {"get_projection_raw", Lua::openvr::lib::get_projection_raw},
	    {"compute_distortion", Lua::openvr::lib::compute_distortion}, {"get_eye_to_head_transform", Lua::openvr::lib::get_eye_to_head_transform}, {"get_time_since_last_vsync", Lua::openvr::lib::get_time_since_last_vsync},
	    {"get_device_to_absolute_tracking_pose", Lua::openvr::lib::get_device_to_absolute_tracking_pose}, {"compute_seconds_to_photons", Lua::openvr::lib::compute_seconds_to_photons},
	    {"get_seated_zero_pose_to_standing_absolute_tracking_pose", Lua::openvr::lib::get_seated_zero_pose_to_standing_absolute_tracking_pose}, {"get_tracked_device_class", Lua::openvr::lib::get_tracked_device_class},
	    {"is_tracked_device_connected", Lua::openvr::lib::is_tracked_device_connected}, {"trigger_haptic_pulse", Lua::openvr::lib::trigger_haptic_pulse}, {"get_controller_state", Lua::openvr::lib::get_controller_state}, {"get_controller_states", Lua::openvr::lib::get_controller_states},
	    {"get_controller_state_with_pose", Lua::openvr::lib::get_controller_state_with_pose}, {"get_controller_role", Lua::openvr::lib::get_controller_role},
	    {"get_tracked_device_serial_number",
	      +[](lua_State *l) {
		      if(s_vrInstance == nullptr)
			      return 0;
		      auto devIndex = Lua::CheckInt(l, 1);
		      auto serialNumber = s_vrInstance->GetTrackedDeviceSerialNumber(devIndex);
		      if(!serialNumber.has_value())
			      return 0;
		      Lua::PushString(l, *serialNumber);
		      return 1;
	      }},
	    {"get_tracked_device_type",
	      +[](lua_State *l) {
		      if(s_vrInstance == nullptr)
			      return 0;
		      auto devIndex = Lua::CheckInt(l, 1);
		      auto serialNumber = s_vrInstance->GetTrackedDeviceType(devIndex);
		      if(!serialNumber.has_value())
			      return 0;
		      Lua::PushString(l, *serialNumber);
		      return 1;
	      }},
	    {"get_pose_transform", Lua::openvr::lib::get_pose_transform}, {"get_pose", Lua::openvr::lib::get_pose}, {"update_poses", static_cast<int32_t (*)(lua_State *)>([](lua_State *l) -> int32_t {
		                                                                                                             if(s_vrInstance)
			                                                                                                             s_vrInstance->UpdateHMDPoses();
		                                                                                                             return 0;
	                                                                                                             })},
	    {"get_hmd_pose_matrix", static_cast<int32_t (*)(lua_State *)>([](lua_State *l) -> int32_t {
		     if(s_vrInstance == nullptr)
			     Lua::Push<Mat4>(l, umat::identity());
		     else
			     Lua::Push<Mat4>(l, s_vrInstance->GetHMDPoseMatrix());
		     return 1;
	     })},
	    {"get_hmd_pose", static_cast<int32_t (*)(lua_State *)>([](lua_State *l) -> int32_t {
		     if(s_vrInstance == nullptr)
			     Lua::Push<umath::Transform>(l, umath::Transform {});
		     else {
			     auto &hmdPoseMatrix = s_vrInstance->GetHMDPoseMatrix();
			     auto pose = openvr_matrix_to_pragma_pose(hmdPoseMatrix);
			     Lua::Push<umath::Transform>(l, pose);
		     }
		     return 1;
	     })},
	    {"get_eye", static_cast<int32_t (*)(lua_State *)>([](lua_State *l) -> int32_t {
		     if(s_vrInstance == nullptr)
			     return 0;
		     auto &eye = get_eye(l, 1);
		     Lua::Push<::openvr::Eye *>(l, const_cast<::openvr::Eye *>(&eye));
		     return 1;
	     })},
	    {"submit_eye", static_cast<int32_t (*)(lua_State *)>([](lua_State *l) -> int32_t {
		     if(s_vrInstance == nullptr) {
			     Lua::PushInt(l, vr::EVRCompositorError::VRCompositorError_None);
			     return 1;
		     }
		     auto &eye = get_eye(l, 1);
		     Lua::PushInt(l, s_vrInstance->GetCompositorInterface()->Submit(eye.GetVREye(), &eye.GetVRTexture()));
		     return 1;
	     })},
	    {"set_eye_image", static_cast<int32_t (*)(lua_State *)>([](lua_State *l) -> int32_t {
		     if(s_vrInstance == nullptr)
			     return 0;
		     auto eyeIndex = static_cast<vr::EVREye>(Lua::CheckInt(l, 1));
		     auto &img = Lua::Check<prosper::IImage>(l, 2);
		     auto &eye = (eyeIndex == vr::EVREye::Eye_Left) ? s_vrInstance->GetLeftEye() : s_vrInstance->GetRightEye();
		     eye.SetImage(img);
		     return 0;
	     })},
	    {"poll_events", static_cast<int32_t (*)(lua_State *)>([](lua_State *l) -> int32_t {
		     auto t = luabind::newtable(l);
		     if(s_vrInstance == nullptr) {
			     t.push(l);
			     return 1;
		     }
		     int32_t idx = 1;
		     for(auto &ev : s_vrInstance->PollEvents()) {
			     auto tEv = luabind::newtable(l);
			     tEv["type"] = ev.eventType;
			     auto data = luabind::newtable(l);
			     tEv["data"] = data;
			     tEv["trackedDeviceIndex"] = ev.trackedDeviceIndex;
			     add_event_data(ev, data);
			     t[idx++] = tEv;
		     }
		     t.push(l);
		     return 1;
	     })},
	    {"is_instance_valid", static_cast<int32_t (*)(lua_State *)>([](lua_State *l) -> int32_t {
		     Lua::Push(l, s_vrInstance != nullptr);
		     return 1;
	     })},
	    /*{"run_openxr_demo",static_cast<int32_t(*)(lua_State*)>([](lua_State *l) -> int32_t {
			std::vector<char*> args = {
				"",
				"-g",
				"Vulkan2",
				"-ff",
				"Hmd",
				"-vc",
				"Stereo",
				"-v"
			};
			run_openxr_demo(args.size(),args.data());
			return 0;
		})}*/
	  });
	//int run_openxr_demo(int argc, char* argv[]) {

	std::unordered_map<std::string, lua_Integer> propErrorEnums {
	  {"TRACKED_PROPERTY_ERROR_SUCCESS", static_cast<int32_t>(vr::ETrackedPropertyError::TrackedProp_Success)},
	  {"TRACKED_PROPERTY_ERROR_WRONG_DATA_TYPE", static_cast<int32_t>(vr::ETrackedPropertyError::TrackedProp_WrongDataType)},
	  {"TRACKED_PROPERTY_ERROR_WRONG_DEVICE_CLASS", static_cast<int32_t>(vr::ETrackedPropertyError::TrackedProp_WrongDeviceClass)},
	  {"TRACKED_PROPERTY_ERROR_BUFFER_TOO_SMALL", static_cast<int32_t>(vr::ETrackedPropertyError::TrackedProp_BufferTooSmall)},
	  {"TRACKED_PROPERTY_ERROR_UNKNOWN_PROPERTY", static_cast<int32_t>(vr::ETrackedPropertyError::TrackedProp_UnknownProperty)},
	  {"TRACKED_PROPERTY_ERROR_INVALID_DEVICE", static_cast<int32_t>(vr::ETrackedPropertyError::TrackedProp_InvalidDevice)},
	  {"TRACKED_PROPERTY_ERROR_COULD_NOT_CONTACT_SERVER", static_cast<int32_t>(vr::ETrackedPropertyError::TrackedProp_CouldNotContactServer)},
	  {"TRACKED_PROPERTY_ERROR_VALUE_NOT_PROVIDED_BY_DEVICE", static_cast<int32_t>(vr::ETrackedPropertyError::TrackedProp_ValueNotProvidedByDevice)},
	  {"TRACKED_PROPERTY_ERROR_STRING_EXCEEDS_MAXIMUM_LENGTH", static_cast<int32_t>(vr::ETrackedPropertyError::TrackedProp_StringExceedsMaximumLength)},
	  {"TRACKED_PROPERTY_ERROR_NOT_YET_AVAILABLE", static_cast<int32_t>(vr::ETrackedPropertyError::TrackedProp_NotYetAvailable)},
	};
	Lua::RegisterLibraryEnums(lua, "openvr", propErrorEnums);

	std::unordered_map<std::string, lua_Integer> propTrackingResults {
	  {"TRACKING_RESULT_UNINITIALIZED", umath::to_integral(vr::ETrackingResult::TrackingResult_Uninitialized)},
	  {"TRACKING_RESULT_CALIBRATING_IN_PROGRESS", umath::to_integral(vr::ETrackingResult::TrackingResult_Calibrating_InProgress)},
	  {"TRACKING_RESULT_CALIBRATING_OUT_OF_RANGE", umath::to_integral(vr::ETrackingResult::TrackingResult_Calibrating_OutOfRange)},
	  {"TRACKING_RESULT_RUNNING_OK", umath::to_integral(vr::ETrackingResult::TrackingResult_Running_OK)},
	  {"TRACKING_RESULT_RUNNING_OUT_OF_RANGE", umath::to_integral(vr::ETrackingResult::TrackingResult_Running_OutOfRange)},
	};
	Lua::RegisterLibraryEnums(lua, "openvr", propTrackingResults);

	std::unordered_map<std::string, lua_Integer> trackedControllerRoles {
	  {"TRACKED_CONTROLLER_ROLE_INVALID", umath::to_integral(vr::ETrackedControllerRole::TrackedControllerRole_Invalid)},
	  {"TRACKED_CONTROLLER_ROLE_LEFT_HAND", umath::to_integral(vr::ETrackedControllerRole::TrackedControllerRole_LeftHand)},
	  {"TRACKED_CONTROLLER_ROLE_RIGHT_HAND", umath::to_integral(vr::ETrackedControllerRole::TrackedControllerRole_RightHand)},
	  {"TRACKED_CONTROLLER_ROLE_OPT_OUT", umath::to_integral(vr::ETrackedControllerRole::TrackedControllerRole_OptOut)},
	  {"TRACKED_CONTROLLER_ROLE_TREADMILL", umath::to_integral(vr::ETrackedControllerRole::TrackedControllerRole_Treadmill)},
	  {"TRACKED_CONTROLLER_ROLE_STYLUS", umath::to_integral(vr::ETrackedControllerRole::TrackedControllerRole_Stylus)},
	  {"TRACKED_CONTROLLER_ROLE_MAX", umath::to_integral(vr::ETrackedControllerRole::TrackedControllerRole_Max)},
	};
	Lua::RegisterLibraryEnums(lua, "openvr", trackedControllerRoles);

	std::unordered_map<std::string, lua_Integer> initErrorEnums {
	  {"INIT_ERROR_NONE", static_cast<int32_t>(vr::EVRInitError::VRInitError_None)},
	  {"INIT_ERROR_UNKNOWN", static_cast<int32_t>(vr::EVRInitError::VRInitError_Unknown)},

	  {"INIT_ERROR_INIT_INSTALLATION_NOT_FOUND", static_cast<int32_t>(vr::EVRInitError::VRInitError_Init_InstallationNotFound)},
	  {"INIT_ERROR_INIT_INSTALLATION_CORRUPT", static_cast<int32_t>(vr::EVRInitError::VRInitError_Init_InstallationCorrupt)},
	  {"INIT_ERROR_INIT_VR_CLIENT_DLL_NOT_FOUND", static_cast<int32_t>(vr::EVRInitError::VRInitError_Init_VRClientDLLNotFound)},
	  {"INIT_ERROR_INIT_FILE_NOT_FOUND", static_cast<int32_t>(vr::EVRInitError::VRInitError_Init_FileNotFound)},
	  {"INIT_ERROR_INIT_FACTORY_NOT_FOUND", static_cast<int32_t>(vr::EVRInitError::VRInitError_Init_FactoryNotFound)},
	  {"INIT_ERROR_INIT_INTERFACE_NOT_FOUND", static_cast<int32_t>(vr::EVRInitError::VRInitError_Init_InterfaceNotFound)},
	  {"INIT_ERROR_INIT_INVALID_INTERFACE", static_cast<int32_t>(vr::EVRInitError::VRInitError_Init_InvalidInterface)},
	  {"INIT_ERROR_INIT_USER_CONFIG_DIRECTORY_INVALID", static_cast<int32_t>(vr::EVRInitError::VRInitError_Init_UserConfigDirectoryInvalid)},
	  {"INIT_ERROR_INIT_HMD_NOT_FOUND", static_cast<int32_t>(vr::EVRInitError::VRInitError_Init_HmdNotFound)},
	  {"INIT_ERROR_INIT_NOT_INITIALIZED", static_cast<int32_t>(vr::EVRInitError::VRInitError_Init_NotInitialized)},
	  {"INIT_ERROR_INIT_PATH_REGISTRY_NOT_FOUND", static_cast<int32_t>(vr::EVRInitError::VRInitError_Init_PathRegistryNotFound)},
	  {"INIT_ERROR_INIT_NO_CONFIG_PATH", static_cast<int32_t>(vr::EVRInitError::VRInitError_Init_NoConfigPath)},
	  {"INIT_ERROR_INIT_NO_LOG_PATH", static_cast<int32_t>(vr::EVRInitError::VRInitError_Init_NoLogPath)},
	  {"INIT_ERROR_INIT_PATH_REGISTRY_NOT_WRITABLE", static_cast<int32_t>(vr::EVRInitError::VRInitError_Init_PathRegistryNotWritable)},
	  {"INIT_ERROR_INIT_APP_INFO_INIT_FAILED", static_cast<int32_t>(vr::EVRInitError::VRInitError_Init_AppInfoInitFailed)},
	  {"INIT_ERROR_INIT_RETRY", static_cast<int32_t>(vr::EVRInitError::VRInitError_Init_Retry)},
	  {"INIT_ERROR_INIT_CANCELED_BY_USER", static_cast<int32_t>(vr::EVRInitError::VRInitError_Init_InitCanceledByUser)},
	  {"INIT_ERROR_INIT_ANOTHER_APP_LAUNCHING", static_cast<int32_t>(vr::EVRInitError::VRInitError_Init_AnotherAppLaunching)},
	  {"INIT_ERROR_INIT_SETTINGS_INIT_FAILED", static_cast<int32_t>(vr::EVRInitError::VRInitError_Init_SettingsInitFailed)},
	  {"INIT_ERROR_INIT_SHUTTING_DOWN", static_cast<int32_t>(vr::EVRInitError::VRInitError_Init_ShuttingDown)},
	  {"INIT_ERROR_INIT_TOO_MANY_OBJECTS", static_cast<int32_t>(vr::EVRInitError::VRInitError_Init_TooManyObjects)},
	  {"INIT_ERROR_INIT_NO_SERVER_FOR_BACKGROUND_APP", static_cast<int32_t>(vr::EVRInitError::VRInitError_Init_NoServerForBackgroundApp)},
	  {"INIT_ERROR_INIT_NOT_SUPPORTED_WITH_COMPOSITOR", static_cast<int32_t>(vr::EVRInitError::VRInitError_Init_NotSupportedWithCompositor)},
	  {"INIT_ERROR_INIT_NOT_AVAILABLE_TO_UTILITY_APPS", static_cast<int32_t>(vr::EVRInitError::VRInitError_Init_NotAvailableToUtilityApps)},

	  {"INIT_ERROR_DRIVER_FAILED", static_cast<int32_t>(vr::EVRInitError::VRInitError_Driver_Failed)},
	  {"INIT_ERROR_DRIVER_UNKNOWN", static_cast<int32_t>(vr::EVRInitError::VRInitError_Driver_Unknown)},
	  {"INIT_ERROR_DRIVER_HMD_UNKNOWN", static_cast<int32_t>(vr::EVRInitError::VRInitError_Driver_HmdUnknown)},
	  {"INIT_ERROR_DRIVER_NOT_LOADED", static_cast<int32_t>(vr::EVRInitError::VRInitError_Driver_NotLoaded)},
	  {"INIT_ERROR_DRIVER_RUNTIME_OUT_OF_DATE", static_cast<int32_t>(vr::EVRInitError::VRInitError_Driver_RuntimeOutOfDate)},
	  {"INIT_ERROR_DRIVER_HMD_IN_USE", static_cast<int32_t>(vr::EVRInitError::VRInitError_Driver_HmdInUse)},
	  {"INIT_ERROR_DRIVER_NOT_CALIBRATED", static_cast<int32_t>(vr::EVRInitError::VRInitError_Driver_NotCalibrated)},
	  {"INIT_ERROR_DRIVER_CALIBRATION_INVALID", static_cast<int32_t>(vr::EVRInitError::VRInitError_Driver_CalibrationInvalid)},
	  {"INIT_ERROR_DRIVER_HMD_DISPLAY_NOT_FOUND", static_cast<int32_t>(vr::EVRInitError::VRInitError_Driver_HmdDisplayNotFound)},

	  {"INIT_ERROR_IPC_SERVER_INIT_FAILED", static_cast<int32_t>(vr::EVRInitError::VRInitError_IPC_ServerInitFailed)},
	  {"INIT_ERROR_IPC_CONNECT_FAILED", static_cast<int32_t>(vr::EVRInitError::VRInitError_IPC_ConnectFailed)},
	  {"INIT_ERROR_IPC_SHARED_STATE_INIT_FAILED", static_cast<int32_t>(vr::EVRInitError::VRInitError_IPC_SharedStateInitFailed)},
	  {"INIT_ERROR_IPC_COMPOSITOR_INIT_FAILED", static_cast<int32_t>(vr::EVRInitError::VRInitError_IPC_CompositorInitFailed)},
	  {"INIT_ERROR_IPC_MUTEX_INIT_FAILED", static_cast<int32_t>(vr::EVRInitError::VRInitError_IPC_MutexInitFailed)},
	  {"INIT_ERROR_IPC_FAILED", static_cast<int32_t>(vr::EVRInitError::VRInitError_IPC_Failed)},

	  {"INIT_ERROR_COMPOSITOR_FAILED", static_cast<int32_t>(vr::EVRInitError::VRInitError_Compositor_Failed)},
	  {"INIT_ERROR_COMPOSITOR_D3D11_HARDWARE_REQUIRED", static_cast<int32_t>(vr::EVRInitError::VRInitError_Compositor_D3D11HardwareRequired)},

	  {"INIT_ERROR_VENDOR_SPECIFIC_UNABLE_TO_CONNECT_TO_OCULUS_RUNTIME", static_cast<int32_t>(vr::EVRInitError::VRInitError_VendorSpecific_UnableToConnectToOculusRuntime)},

	  {"INIT_ERROR_VENDOR_SPECIFIC_HMD_FOUND_CANT_OPEN_DEVICE", static_cast<int32_t>(vr::EVRInitError::VRInitError_VendorSpecific_HmdFound_CantOpenDevice)},
	  {"INIT_ERROR_VENDOR_SPECIFIC_HMD_FOUND_UNABLE_TO_REQUEST_CONFIG_START", static_cast<int32_t>(vr::EVRInitError::VRInitError_VendorSpecific_HmdFound_UnableToRequestConfigStart)},
	  {"INIT_ERROR_VENDOR_SPECIFIC_HMD_FOUND_NO_STORED_CONFIG", static_cast<int32_t>(vr::EVRInitError::VRInitError_VendorSpecific_HmdFound_NoStoredConfig)},
	  {"INIT_ERROR_VENDOR_SPECIFIC_HMD_FOUND_CONFIG_TOO_BIG", static_cast<int32_t>(vr::EVRInitError::VRInitError_VendorSpecific_HmdFound_ConfigTooBig)},
	  {"INIT_ERROR_VENDOR_SPECIFIC_HMD_FOUND_CONFIG_TOO_SMALL", static_cast<int32_t>(vr::EVRInitError::VRInitError_VendorSpecific_HmdFound_ConfigTooSmall)},
	  {"INIT_ERROR_VENDOR_SPECIFIC_HMD_FOUND_UNABLE_TO_INIT_ZLIB", static_cast<int32_t>(vr::EVRInitError::VRInitError_VendorSpecific_HmdFound_UnableToInitZLib)},
	  {"INIT_ERROR_VENDOR_SPECIFIC_HMD_FOUND_CANT_READ_FIRMWARE_VERSION", static_cast<int32_t>(vr::EVRInitError::VRInitError_VendorSpecific_HmdFound_CantReadFirmwareVersion)},
	  {"INIT_ERROR_VENDOR_SPECIFIC_HMD_FOUND_UNABLE_TO_SEND_USER_DATA_START", static_cast<int32_t>(vr::EVRInitError::VRInitError_VendorSpecific_HmdFound_UnableToSendUserDataStart)},
	  {"INIT_ERROR_VENDOR_SPECIFIC_HMD_FOUND_UNABLE_TO_GET_USER_DATA_START", static_cast<int32_t>(vr::EVRInitError::VRInitError_VendorSpecific_HmdFound_UnableToGetUserDataStart)},
	  {"INIT_ERROR_VENDOR_SPECIFIC_HMD_FOUND_UNABLE_TO_GET_USER_DATA_NEXT", static_cast<int32_t>(vr::EVRInitError::VRInitError_VendorSpecific_HmdFound_UnableToGetUserDataNext)},
	  {"INIT_ERROR_VENDOR_SPECIFIC_HMD_FOUND_USER_DATA_ADDRESS_RANGE", static_cast<int32_t>(vr::EVRInitError::VRInitError_VendorSpecific_HmdFound_UserDataAddressRange)},
	  {"INIT_ERROR_VENDOR_SPECIFIC_HMD_FOUND_USER_DATA_ERROR", static_cast<int32_t>(vr::EVRInitError::VRInitError_VendorSpecific_HmdFound_UserDataError)},
	  {"INIT_ERROR_VENDOR_SPECIFIC_HMD_FOUND_CONFIG_FAILED_SANITY_CHECK", static_cast<int32_t>(vr::EVRInitError::VRInitError_VendorSpecific_HmdFound_ConfigFailedSanityCheck)},

	  {"INIT_ERROR_STEAM_INSTALLATION_NOT_FOUND", static_cast<int32_t>(vr::EVRInitError::VRInitError_Steam_SteamInstallationNotFound)},

	  {"COMPOSITOR_ERROR_NONE", static_cast<int32_t>(vr::EVRCompositorError::VRCompositorError_None)},
	  {"COMPOSITOR_ERROR_REQUEST_FAILED", static_cast<int32_t>(vr::EVRCompositorError::VRCompositorError_RequestFailed)},
	  {"COMPOSITOR_ERROR_INCOMPATIBLE_VERSION", static_cast<int32_t>(vr::EVRCompositorError::VRCompositorError_IncompatibleVersion)},
	  {"COMPOSITOR_ERROR_DO_NOT_HAVE_FOCUS", static_cast<int32_t>(vr::EVRCompositorError::VRCompositorError_DoNotHaveFocus)},
	  {"COMPOSITOR_ERROR_INVALID_TEXTURE", static_cast<int32_t>(vr::EVRCompositorError::VRCompositorError_InvalidTexture)},
	  {"COMPOSITOR_ERROR_IS_NOT_SCENE_APPLICATION", static_cast<int32_t>(vr::EVRCompositorError::VRCompositorError_IsNotSceneApplication)},
	  {"COMPOSITOR_ERROR_TEXTURE_IS_ON_WRONG_DEVICE", static_cast<int32_t>(vr::EVRCompositorError::VRCompositorError_TextureIsOnWrongDevice)},
	  {"COMPOSITOR_ERROR_TEXTURE_USES_UNSUPPORTED_FORMAT", static_cast<int32_t>(vr::EVRCompositorError::VRCompositorError_TextureUsesUnsupportedFormat)},
	  {"COMPOSITOR_ERROR_SHARED_TEXTURES_NOT_SUPPORTED", static_cast<int32_t>(vr::EVRCompositorError::VRCompositorError_SharedTexturesNotSupported)},
	  {"COMPOSITOR_ERROR_INDEX_OUT_OF_RANGE", static_cast<int32_t>(vr::EVRCompositorError::VRCompositorError_IndexOutOfRange)},
	  {"COMPOSITOR_ERROR_ALREADY_SUBMITTED", static_cast<int32_t>(vr::EVRCompositorError::VRCompositorError_AlreadySubmitted)},

	  {"TRACKING_UNIVERSE_ORIGIN_SEATED", static_cast<int32_t>(vr::ETrackingUniverseOrigin::TrackingUniverseSeated)},
	  {"TRACKING_UNIVERSE_ORIGIN_STANDING", static_cast<int32_t>(vr::ETrackingUniverseOrigin::TrackingUniverseStanding)},
	  {"TRACKING_UNIVERSE_ORIGIN_RAW_AND_UNCALIBRATED", static_cast<int32_t>(vr::ETrackingUniverseOrigin::TrackingUniverseRawAndUncalibrated)},

	  {"EYE_LEFT", umath::to_integral(vr::EVREye::Eye_Left)},
	  {"EYE_RIGHT", umath::to_integral(vr::EVREye::Eye_Right)},

	  {"TRACKED_DEVICE_CLASS_INVALID", umath::to_integral(vr::ETrackedDeviceClass::TrackedDeviceClass_Invalid)},
	  {"TRACKED_DEVICE_CLASS_HMD", umath::to_integral(vr::ETrackedDeviceClass::TrackedDeviceClass_HMD)},
	  {"TRACKED_DEVICE_CLASS_CONTROLLER", umath::to_integral(vr::ETrackedDeviceClass::TrackedDeviceClass_Controller)},
	  {"TRACKED_DEVICE_CLASS_GENERIC_TRACKER", umath::to_integral(vr::ETrackedDeviceClass::TrackedDeviceClass_GenericTracker)},
	  {"TRACKED_DEVICE_CLASS_TRACKING_REFERENCE", umath::to_integral(vr::ETrackedDeviceClass::TrackedDeviceClass_TrackingReference)},

	  {"BUTTON_ID_SYSTEM", static_cast<int32_t>(vr::EVRButtonId::k_EButton_System)},
	  {"BUTTON_ID_APPLICATION_MENU", static_cast<int32_t>(vr::EVRButtonId::k_EButton_ApplicationMenu)},
	  {"BUTTON_ID_GRIP", static_cast<int32_t>(vr::EVRButtonId::k_EButton_Grip)},
	  {"BUTTON_ID_DPAD_LEFT", static_cast<int32_t>(vr::EVRButtonId::k_EButton_DPad_Left)},
	  {"BUTTON_ID_DPAD_UP", static_cast<int32_t>(vr::EVRButtonId::k_EButton_DPad_Up)},
	  {"BUTTON_ID_DPAD_RIGHT", static_cast<int32_t>(vr::EVRButtonId::k_EButton_DPad_Right)},
	  {"BUTTON_ID_DPAD_DOWN", static_cast<int32_t>(vr::EVRButtonId::k_EButton_DPad_Down)},
	  {"BUTTON_ID_A", static_cast<int32_t>(vr::EVRButtonId::k_EButton_A)},
	  {"BUTTON_ID_PROXIMITY_SENSOR", static_cast<int32_t>(vr::EVRButtonId::k_EButton_ProximitySensor)},
	  {"BUTTON_ID_AXIS0", static_cast<int32_t>(vr::EVRButtonId::k_EButton_Axis0)},
	  {"BUTTON_ID_AXIS1", static_cast<int32_t>(vr::EVRButtonId::k_EButton_Axis1)},
	  {"BUTTON_ID_AXIS2", static_cast<int32_t>(vr::EVRButtonId::k_EButton_Axis2)},
	  {"BUTTON_ID_AXIS3", static_cast<int32_t>(vr::EVRButtonId::k_EButton_Axis3)},
	  {"BUTTON_ID_AXIS4", static_cast<int32_t>(vr::EVRButtonId::k_EButton_Axis4)},
	  {"BUTTON_ID_STEAMVR_TOUCHPAD", static_cast<int32_t>(vr::EVRButtonId::k_EButton_SteamVR_Touchpad)},
	  {"BUTTON_ID_STEAMVR_TRIGGER", static_cast<int32_t>(vr::EVRButtonId::k_EButton_SteamVR_Trigger)},
	  {"BUTTON_ID_DASHBOARD_BACK", static_cast<int32_t>(vr::EVRButtonId::k_EButton_Dashboard_Back)},

	  {"CONTROLLER_AXIS_TYPE_NONE", static_cast<int32_t>(vr::EVRControllerAxisType::k_eControllerAxis_None)},
	  {"CONTROLLER_AXIS_TYPE_TRACK_PAD", static_cast<int32_t>(vr::EVRControllerAxisType::k_eControllerAxis_TrackPad)},
	  {"CONTROLLER_AXIS_TYPE_JOYSTICK", static_cast<int32_t>(vr::EVRControllerAxisType::k_eControllerAxis_Joystick)},
	  {"CONTROLLER_AXIS_TYPE_TRIGGER", static_cast<int32_t>(vr::EVRControllerAxisType::k_eControllerAxis_Trigger)},

	  {"MAX_TRACKED_DEVICE_COUNT", vr::k_unMaxTrackedDeviceCount},

	  {"EVENT_NONE", vr::VREvent_None},
	  {"EVENT_TRACKED_DEVICE_ACTIVATED", vr::VREvent_TrackedDeviceActivated},
	  {"EVENT_TRACKED_DEVICE_DEACTIVATED", vr::VREvent_TrackedDeviceDeactivated},
	  {"EVENT_TRACKED_DEVICE_UPDATED", vr::VREvent_TrackedDeviceUpdated},
	  {"EVENT_TRACKED_DEVICE_USER_INTERACTION_STARTED", vr::VREvent_TrackedDeviceUserInteractionStarted},
	  {"EVENT_TRACKED_DEVICE_USER_INTERACTION_ENDED", vr::VREvent_TrackedDeviceUserInteractionEnded},
	  {"EVENT_IPD_CHANGED", vr::VREvent_IpdChanged},
	  {"EVENT_ENTER_STANDBY_MODE", vr::VREvent_EnterStandbyMode},
	  {"EVENT_LEAVE_STANDBY_MODE", vr::VREvent_LeaveStandbyMode},
	  {"EVENT_TRACKED_DEVICE_ROLE_CHANGED", vr::VREvent_TrackedDeviceRoleChanged},
	  {"EVENT_WATCHDOG_WAKE_UP_REQUESTED", vr::VREvent_WatchdogWakeUpRequested},
	  {"EVENT_LENS_DISTORTION_CHANGED", vr::VREvent_LensDistortionChanged},
	  {"EVENT_PROPERTY_CHANGED", vr::VREvent_PropertyChanged},
	  {"EVENT_WIRELESS_DISCONNECT", vr::VREvent_WirelessDisconnect},
	  {"EVENT_WIRELESS_RECONNECT", vr::VREvent_WirelessReconnect},
	  {"EVENT_BUTTON_PRESS", vr::VREvent_ButtonPress},
	  {"EVENT_BUTTON_UNPRESS", vr::VREvent_ButtonUnpress},
	  {"EVENT_BUTTON_TOUCH", vr::VREvent_ButtonTouch},
	  {"EVENT_BUTTON_UNTOUCH", vr::VREvent_ButtonUntouch},
	  {"EVENT_MODAL_CANCEL", vr::VREvent_Modal_Cancel},
	  {"EVENT_MOUSE_MOVE", vr::VREvent_MouseMove},
	  {"EVENT_MOUSE_BUTTON_DOWN", vr::VREvent_MouseButtonDown},
	  {"EVENT_MOUSE_BUTTON_UP", vr::VREvent_MouseButtonUp},
	  {"EVENT_FOCUS_ENTER", vr::VREvent_FocusEnter},
	  {"EVENT_FOCUS_LEAVE", vr::VREvent_FocusLeave},
	  {"EVENT_SCROLL_DISCRETE", vr::VREvent_ScrollDiscrete},
	  {"EVENT_TOUCH_PAD_MOVE", vr::VREvent_TouchPadMove},
	  {"EVENT_OVERLAY_FOCUS_CHANGED", vr::VREvent_OverlayFocusChanged},
	  {"EVENT_RELOAD_OVERLAYS", vr::VREvent_ReloadOverlays},
	  {"EVENT_SCROLL_SMOOTH", vr::VREvent_ScrollSmooth},
	  {"EVENT_LOCK_MOUSE_POSITION", vr::VREvent_LockMousePosition},
	  {"EVENT_UNLOCK_MOUSE_POSITION", vr::VREvent_UnlockMousePosition},
	  {"EVENT_INPUT_FOCUS_CAPTURED", vr::VREvent_InputFocusCaptured},
	  {"EVENT_INPUT_FOCUS_RELEASED", vr::VREvent_InputFocusReleased},
	  {"EVENT_SCENE_APPLICATION_CHANGED", vr::VREvent_SceneApplicationChanged},
	  {"EVENT_INPUT_FOCUS_CHANGED", vr::VREvent_InputFocusChanged},
	  {"EVENT_SCENE_APPLICATION_USING_WRONG_GRAPHICS_ADAPTER", vr::VREvent_SceneApplicationUsingWrongGraphicsAdapter},
	  {"EVENT_ACTION_BINDING_RELOADED", vr::VREvent_ActionBindingReloaded},
	  {"EVENT_HIDE_RENDER_MODELS", vr::VREvent_HideRenderModels},
	  {"EVENT_SHOW_RENDER_MODELS", vr::VREvent_ShowRenderModels},
	  {"EVENT_SCENE_APPLICATION_STATE_CHANGED", vr::VREvent_SceneApplicationStateChanged},
	  {"EVENT_CONSOLE_OPENED", vr::VREvent_ConsoleOpened},
	  {"EVENT_CONSOLE_CLOSED", vr::VREvent_ConsoleClosed},
	  {"EVENT_OVERLAY_SHOWN", vr::VREvent_OverlayShown},
	  {"EVENT_OVERLAY_HIDDEN", vr::VREvent_OverlayHidden},
	  {"EVENT_DASHBOARD_ACTIVATED", vr::VREvent_DashboardActivated},
	  {"EVENT_DASHBOARD_DEACTIVATED", vr::VREvent_DashboardDeactivated},
	  {"EVENT_DASHBOARD_REQUESTED", vr::VREvent_DashboardRequested},
	  {"EVENT_RESET_DASHBOARD", vr::VREvent_ResetDashboard},
	  {"EVENT_IMAGE_LOADED", vr::VREvent_ImageLoaded},
	  {"EVENT_SHOW_KEYBOARD", vr::VREvent_ShowKeyboard},
	  {"EVENT_HIDE_KEYBOARD", vr::VREvent_HideKeyboard},
	  {"EVENT_OVERLAY_GAMEPAD_FOCUS_GAINED", vr::VREvent_OverlayGamepadFocusGained},
	  {"EVENT_OVERLAY_GAMEPAD_FOCUS_LOST", vr::VREvent_OverlayGamepadFocusLost},
	  {"EVENT_OVERLAY_SHARED_TEXTURE_CHANGED", vr::VREvent_OverlaySharedTextureChanged},
	  {"EVENT_SCREENSHOT_TRIGGERED", vr::VREvent_ScreenshotTriggered},
	  {"EVENT_IMAGE_FAILED", vr::VREvent_ImageFailed},
	  {"EVENT_DASHBOARD_OVERLAY_CREATED", vr::VREvent_DashboardOverlayCreated},
	  {"EVENT_SWITCH_GAMEPAD_FOCUS", vr::VREvent_SwitchGamepadFocus},
	  {"EVENT_REQUEST_SCREENSHOT", vr::VREvent_RequestScreenshot},
	  {"EVENT_SCREENSHOT_TAKEN", vr::VREvent_ScreenshotTaken},
	  {"EVENT_SCREENSHOT_FAILED", vr::VREvent_ScreenshotFailed},
	  {"EVENT_SUBMIT_SCREENSHOT_TO_DASHBOARD", vr::VREvent_SubmitScreenshotToDashboard},
	  {"EVENT_SCREENSHOT_PROGRESS_TO_DASHBOARD", vr::VREvent_ScreenshotProgressToDashboard},
	  {"EVENT_PRIMARY_DASHBOARD_DEVICE_CHANGED", vr::VREvent_PrimaryDashboardDeviceChanged},
	  {"EVENT_ROOM_VIEW_SHOWN", vr::VREvent_RoomViewShown},
	  {"EVENT_ROOM_VIEW_HIDDEN", vr::VREvent_RoomViewHidden},
	  {"EVENT_SHOW_UI", vr::VREvent_ShowUI},
	  {"EVENT_SHOW_DEV_TOOLS", vr::VREvent_ShowDevTools},
	  {"EVENT_DESKTOP_VIEW_UPDATING", vr::VREvent_DesktopViewUpdating},
	  {"EVENT_DESKTOP_VIEW_READY", vr::VREvent_DesktopViewReady},
	  {"EVENT_NOTIFICATION_SHOWN", vr::VREvent_Notification_Shown},
	  {"EVENT_NOTIFICATION_HIDDEN", vr::VREvent_Notification_Hidden},
	  {"EVENT_NOTIFICATION_BEGIN_INTERACTION", vr::VREvent_Notification_BeginInteraction},
	  {"EVENT_NOTIFICATION_DESTROYED", vr::VREvent_Notification_Destroyed},
	  {"EVENT_QUIT", vr::VREvent_Quit},
	  {"EVENT_PROCESS_QUIT", vr::VREvent_ProcessQuit},
	  {"EVENT_QUIT_ACKNOWLEDGED", vr::VREvent_QuitAcknowledged},
	  {"EVENT_DRIVER_REQUESTED_QUIT", vr::VREvent_DriverRequestedQuit},
	  {"EVENT_RESTART_REQUESTED", vr::VREvent_RestartRequested},
	  {"EVENT_CHAPERONE_DATA_HAS_CHANGED", vr::VREvent_ChaperoneDataHasChanged},
	  {"EVENT_CHAPERONE_UNIVERSE_HAS_CHANGED", vr::VREvent_ChaperoneUniverseHasChanged},
	  {"EVENT_CHAPERONE_TEMP_DATA_HAS_CHANGED", vr::VREvent_ChaperoneTempDataHasChanged},
	  {"EVENT_CHAPERONE_SETTINGS_HAVE_CHANGED", vr::VREvent_ChaperoneSettingsHaveChanged},
	  {"EVENT_SEATED_ZERO_POSE_RESET", vr::VREvent_SeatedZeroPoseReset},
	  {"EVENT_CHAPERONE_FLUSH_CACHE", vr::VREvent_ChaperoneFlushCache},
	  {"EVENT_CHAPERONE_ROOM_SETUP_STARTING", vr::VREvent_ChaperoneRoomSetupStarting},
	  {"EVENT_CHAPERONE_ROOM_SETUP_FINISHED", vr::VREvent_ChaperoneRoomSetupFinished},
	  {"EVENT_STANDING_ZERO_POSE_RESET", vr::VREvent_StandingZeroPoseReset},
	  {"EVENT_AUDIO_SETTINGS_HAVE_CHANGED", vr::VREvent_AudioSettingsHaveChanged},
	  {"EVENT_BACKGROUND_SETTING_HAS_CHANGED", vr::VREvent_BackgroundSettingHasChanged},
	  {"EVENT_CAMERA_SETTINGS_HAVE_CHANGED", vr::VREvent_CameraSettingsHaveChanged},
	  {"EVENT_REPROJECTION_SETTING_HAS_CHANGED", vr::VREvent_ReprojectionSettingHasChanged},
	  {"EVENT_MODEL_SKIN_SETTINGS_HAVE_CHANGED", vr::VREvent_ModelSkinSettingsHaveChanged},
	  {"EVENT_ENVIRONMENT_SETTINGS_HAVE_CHANGED", vr::VREvent_EnvironmentSettingsHaveChanged},
	  {"EVENT_POWER_SETTINGS_HAVE_CHANGED", vr::VREvent_PowerSettingsHaveChanged},
	  {"EVENT_ENABLE_HOME_APP_SETTINGS_HAVE_CHANGED", vr::VREvent_EnableHomeAppSettingsHaveChanged},
	  {"EVENT_STEAM_VR_SECTION_SETTING_CHANGED", vr::VREvent_SteamVRSectionSettingChanged},
	  {"EVENT_LIGHTHOUSE_SECTION_SETTING_CHANGED", vr::VREvent_LighthouseSectionSettingChanged},
	  {"EVENT_NULL_SECTION_SETTING_CHANGED", vr::VREvent_NullSectionSettingChanged},
	  {"EVENT_USER_INTERFACE_SECTION_SETTING_CHANGED", vr::VREvent_UserInterfaceSectionSettingChanged},
	  {"EVENT_NOTIFICATIONS_SECTION_SETTING_CHANGED", vr::VREvent_NotificationsSectionSettingChanged},
	  {"EVENT_KEYBOARD_SECTION_SETTING_CHANGED", vr::VREvent_KeyboardSectionSettingChanged},
	  {"EVENT_PERF_SECTION_SETTING_CHANGED", vr::VREvent_PerfSectionSettingChanged},
	  {"EVENT_DASHBOARD_SECTION_SETTING_CHANGED", vr::VREvent_DashboardSectionSettingChanged},
	  {"EVENT_WEB_INTERFACE_SECTION_SETTING_CHANGED", vr::VREvent_WebInterfaceSectionSettingChanged},
	  {"EVENT_TRACKERS_SECTION_SETTING_CHANGED", vr::VREvent_TrackersSectionSettingChanged},
	  {"EVENT_LAST_KNOWN_SECTION_SETTING_CHANGED", vr::VREvent_LastKnownSectionSettingChanged},
	  {"EVENT_DISMISSED_WARNINGS_SECTION_SETTING_CHANGED", vr::VREvent_DismissedWarningsSectionSettingChanged},
	  {"EVENT_GPU_SPEED_SECTION_SETTING_CHANGED", vr::VREvent_GpuSpeedSectionSettingChanged},
	  {"EVENT_WINDOWS_MR_SECTION_SETTING_CHANGED", vr::VREvent_WindowsMRSectionSettingChanged},
	  {"EVENT_OTHER_SECTION_SETTING_CHANGED", vr::VREvent_OtherSectionSettingChanged},
	  {"EVENT_STATUS_UPDATE", vr::VREvent_StatusUpdate},
	  {"EVENT_WEB_INTERFACE_INSTALL_DRIVER_COMPLETED", vr::VREvent_WebInterface_InstallDriverCompleted},
	  {"EVENT_MC_IMAGE_UPDATED", vr::VREvent_MCImageUpdated},
	  {"EVENT_FIRMWARE_UPDATE_STARTED", vr::VREvent_FirmwareUpdateStarted},
	  {"EVENT_FIRMWARE_UPDATE_FINISHED", vr::VREvent_FirmwareUpdateFinished},
	  {"EVENT_KEYBOARD_CLOSED", vr::VREvent_KeyboardClosed},
	  {"EVENT_KEYBOARD_CHAR_INPUT", vr::VREvent_KeyboardCharInput},
	  {"EVENT_KEYBOARD_DONE", vr::VREvent_KeyboardDone},
	  {"EVENT_APPLICATION_LIST_UPDATED", vr::VREvent_ApplicationListUpdated},
	  {"EVENT_APPLICATION_MIME_TYPE_LOAD", vr::VREvent_ApplicationMimeTypeLoad},
	  {"EVENT_PROCESS_CONNECTED", vr::VREvent_ProcessConnected},
	  {"EVENT_PROCESS_DISCONNECTED", vr::VREvent_ProcessDisconnected},
	  {"EVENT_COMPOSITOR_CHAPERONE_BOUNDS_SHOWN", vr::VREvent_Compositor_ChaperoneBoundsShown},
	  {"EVENT_COMPOSITOR_CHAPERONE_BOUNDS_HIDDEN", vr::VREvent_Compositor_ChaperoneBoundsHidden},
	  {"EVENT_COMPOSITOR_DISPLAY_DISCONNECTED", vr::VREvent_Compositor_DisplayDisconnected},
	  {"EVENT_COMPOSITOR_DISPLAY_RECONNECTED", vr::VREvent_Compositor_DisplayReconnected},
	  {"EVENT_COMPOSITOR_HDCP_ERROR", vr::VREvent_Compositor_HDCPError},
	  {"EVENT_COMPOSITOR_APPLICATION_NOT_RESPONDING", vr::VREvent_Compositor_ApplicationNotResponding},
	  {"EVENT_COMPOSITOR_APPLICATION_RESUMED", vr::VREvent_Compositor_ApplicationResumed},
	  {"EVENT_COMPOSITOR_OUT_OF_VIDEO_MEMORY", vr::VREvent_Compositor_OutOfVideoMemory},
	  {"EVENT_COMPOSITOR_DISPLAY_MODE_NOT_SUPPORTED", vr::VREvent_Compositor_DisplayModeNotSupported},
	  {"EVENT_COMPOSITOR_STAGE_OVERRIDE_READY", vr::VREvent_Compositor_StageOverrideReady},
	  {"EVENT_TRACKED_CAMERA_START_VIDEO_STREAM", vr::VREvent_TrackedCamera_StartVideoStream},
	  {"EVENT_TRACKED_CAMERA_STOP_VIDEO_STREAM", vr::VREvent_TrackedCamera_StopVideoStream},
	  {"EVENT_TRACKED_CAMERA_PAUSE_VIDEO_STREAM", vr::VREvent_TrackedCamera_PauseVideoStream},
	  {"EVENT_TRACKED_CAMERA_RESUME_VIDEO_STREAM", vr::VREvent_TrackedCamera_ResumeVideoStream},
	  {"EVENT_TRACKED_CAMERA_EDITING_SURFACE", vr::VREvent_TrackedCamera_EditingSurface},
	  {"EVENT_PERFORMANCE_TEST_ENABLE_CAPTURE", vr::VREvent_PerformanceTest_EnableCapture},
	  {"EVENT_PERFORMANCE_TEST_DISABLE_CAPTURE", vr::VREvent_PerformanceTest_DisableCapture},
	  {"EVENT_PERFORMANCE_TEST_FIDELITY_LEVEL", vr::VREvent_PerformanceTest_FidelityLevel},
	  {"EVENT_MESSAGE_OVERLAY_CLOSED", vr::VREvent_MessageOverlay_Closed},
	  {"EVENT_MESSAGE_OVERLAY_CLOSE_REQUESTED", vr::VREvent_MessageOverlayCloseRequested},
	  {"EVENT_INPUT_HAPTIC_VIBRATION", vr::VREvent_Input_HapticVibration},
	  {"EVENT_INPUT_BINDING_LOAD_FAILED", vr::VREvent_Input_BindingLoadFailed},
	  {"EVENT_INPUT_BINDING_LOAD_SUCCESSFUL", vr::VREvent_Input_BindingLoadSuccessful},
	  {"EVENT_INPUT_ACTION_MANIFEST_RELOADED", vr::VREvent_Input_ActionManifestReloaded},
	  {"EVENT_INPUT_ACTION_MANIFEST_LOAD_FAILED", vr::VREvent_Input_ActionManifestLoadFailed},
	  {"EVENT_INPUT_PROGRESS_UPDATE", vr::VREvent_Input_ProgressUpdate},
	  {"EVENT_INPUT_TRACKER_ACTIVATED", vr::VREvent_Input_TrackerActivated},
	  {"EVENT_INPUT_BINDINGS_UPDATED", vr::VREvent_Input_BindingsUpdated},
	  {"EVENT_INPUT_BINDING_SUBSCRIPTION_CHANGED", vr::VREvent_Input_BindingSubscriptionChanged},
	  {"EVENT_SPATIAL_ANCHORS_POSE_UPDATED", vr::VREvent_SpatialAnchors_PoseUpdated},
	  {"EVENT_SPATIAL_ANCHORS_DESCRIPTOR_UPDATED", vr::VREvent_SpatialAnchors_DescriptorUpdated},
	  {"EVENT_SPATIAL_ANCHORS_REQUEST_POSE_UPDATE", vr::VREvent_SpatialAnchors_RequestPoseUpdate},
	  {"EVENT_SPATIAL_ANCHORS_REQUEST_DESCRIPTOR_UPDATE", vr::VREvent_SpatialAnchors_RequestDescriptorUpdate},
	  {"EVENT_SYSTEM_REPORT_STARTED", vr::VREvent_SystemReport_Started},
	  {"EVENT_MONITOR_SHOW_HEADSET_VIEW", vr::VREvent_Monitor_ShowHeadsetView},
	  {"EVENT_MONITOR_HIDE_HEADSET_VIEW", vr::VREvent_Monitor_HideHeadsetView},
	};
	Lua::RegisterLibraryEnums(lua, "openvr", initErrorEnums);

	auto &modVr = l.RegisterLibrary("openvr");
	modVr[luabind::def(
	  "reset_zero_pose", +[](vr::ETrackingUniverseOrigin origin) {
		  if(s_vrInstance == nullptr)
			  return;
		  auto *chaperone = s_vrInstance->GetChaperone();
		  chaperone->ResetZeroPose(origin);
	  })];

	auto classDevDevicePose = luabind::class_<vr::TrackedDevicePose_t>("TrackedDevicePose")
	                            .def_readonly("deviceIsConnected", &vr::TrackedDevicePose_t::bDeviceIsConnected)
	                            .def_readonly("poseIsValid", &vr::TrackedDevicePose_t::bPoseIsValid)
	                            .def_readonly("trackingResult", &vr::TrackedDevicePose_t::eTrackingResult)
	                            .def_readonly("deviceToAbsoluteTracking", reinterpret_cast<Mat3x4 vr::TrackedDevicePose_t::*>(&vr::TrackedDevicePose_t::mDeviceToAbsoluteTracking))
	                            .def_readonly("angularVelocity", reinterpret_cast<Vector3 vr::TrackedDevicePose_t::*>(&vr::TrackedDevicePose_t::vAngularVelocity))
	                            .def_readonly("velocity", reinterpret_cast<Vector3 vr::TrackedDevicePose_t::*>(&vr::TrackedDevicePose_t::vVelocity));
	modVr[classDevDevicePose];

	auto classDefEye = luabind::class_<::openvr::Eye>("Eye");
	classDefEye.def("GetProjectionMatrix", static_cast<void (*)(lua_State *, ::openvr::Eye &, float, float)>([](lua_State *l, ::openvr::Eye &eye, float nearZ, float farZ) { Lua::Push<Mat4>(l, eye.GetEyeProjectionMatrix(nearZ, farZ)); }));
	classDefEye.def("GetViewMatrix", static_cast<void (*)(lua_State *, ::openvr::Eye &, pragma::CCameraComponent &)>([](lua_State *l, ::openvr::Eye &eye, pragma::CCameraComponent &cam) { Lua::Push<Mat4>(l, eye.GetEyeViewMatrix(cam)); }));
	modVr[classDefEye];

	auto classDefControllerState = luabind::class_<LuaVRControllerState>("ControllerState")
	                                 .def_readonly("packetNum", &LuaVRControllerState::unPacketNum)
	                                 .def_readonly("buttonPressed", &LuaVRControllerState::ulButtonPressed)
	                                 .def_readonly("buttonTouched", &LuaVRControllerState::ulButtonTouched)
	                                 .def_readonly("axis0", reinterpret_cast<Vector2 LuaVRControllerState::*>(&LuaVRControllerState::rAxis0))
	                                 .def_readonly("axis1", reinterpret_cast<Vector2 LuaVRControllerState::*>(&LuaVRControllerState::rAxis1))
	                                 .def_readonly("axis2", reinterpret_cast<Vector2 LuaVRControllerState::*>(&LuaVRControllerState::rAxis2))
	                                 .def_readonly("axis3", reinterpret_cast<Vector2 LuaVRControllerState::*>(&LuaVRControllerState::rAxis3))
	                                 .def_readonly("axis4", reinterpret_cast<Vector2 LuaVRControllerState::*>(&LuaVRControllerState::rAxis4));
	modVr[classDefControllerState];
}
