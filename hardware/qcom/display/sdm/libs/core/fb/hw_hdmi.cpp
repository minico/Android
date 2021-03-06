/*
* Copyright (c) 2015, The Linux Foundation. All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are
* met:
*     * Redistributions of source code must retain the above copyright
*       notice, this list of conditions and the following disclaimer.
*     * Redistributions in binary form must reproduce the above
*       copyright notice, this list of conditions and the following
*       disclaimer in the documentation and/or other materials provided
*       with the distribution.
*     * Neither the name of The Linux Foundation nor the names of its
*       contributors may be used to endorse or promote products derived
*       from this software without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
* WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
* ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
* BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
* CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
* SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
* BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
* WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
* OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
* IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <ctype.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <utils/debug.h>
#include <utils/sys.h>

#include "hw_hdmi.h"

#define __CLASS__ "HWHDMI"

namespace sdm {

static bool MapHDMIDisplayTiming(const msm_hdmi_mode_timing_info *mode,
                                 fb_var_screeninfo *info) {
  if (!mode || !info) {
    return false;
  }

  info->reserved[0] = 0;
  info->reserved[1] = 0;
  info->reserved[2] = 0;
  info->reserved[3] = (info->reserved[3] & 0xFFFF) | (mode->video_format << 16);
  info->xoffset = 0;
  info->yoffset = 0;
  info->xres = mode->active_h;
  info->yres = mode->active_v;
  info->pixclock = (mode->pixel_freq) * 1000;
  info->vmode = mode->interlaced ? FB_VMODE_INTERLACED : FB_VMODE_NONINTERLACED;
  info->right_margin = mode->front_porch_h;
  info->hsync_len = mode->pulse_width_h;
  info->left_margin = mode->back_porch_h;
  info->lower_margin = mode->front_porch_v;
  info->vsync_len = mode->pulse_width_v;
  info->upper_margin = mode->back_porch_v;

  info->grayscale = V4L2_PIX_FMT_RGB24;
  // If the mode supports YUV420 set grayscale to the FOURCC value for YUV420.
  if (IS_BIT_SET(mode->pixel_formats, 1)) {
    info->grayscale = V4L2_PIX_FMT_NV12;
  }

  return true;
}

DisplayError HWHDMI::Create(HWInterface **intf, HWInfoInterface *hw_info_intf,
                            BufferSyncHandler *buffer_sync_handler) {
  DisplayError error = kErrorNone;
  HWHDMI *hw_fb_hdmi = NULL;

  hw_fb_hdmi = new HWHDMI(buffer_sync_handler, hw_info_intf);
  error = hw_fb_hdmi->Init(NULL);
  if (error != kErrorNone) {
    delete hw_fb_hdmi;
  } else {
    *intf = hw_fb_hdmi;
  }
  return error;
}

DisplayError HWHDMI::Destroy(HWInterface *intf) {
  HWHDMI *hw_fb_hdmi = static_cast<HWHDMI *>(intf);
  hw_fb_hdmi->Deinit();
  delete hw_fb_hdmi;

  return kErrorNone;
}

HWHDMI::HWHDMI(BufferSyncHandler *buffer_sync_handler,  HWInfoInterface *hw_info_intf)
  : HWDevice(buffer_sync_handler), hw_scan_info_(), active_config_index_(0) {
  HWDevice::device_type_ = kDeviceHDMI;
  HWDevice::device_name_ = "HDMI Display Device";
  HWDevice::hw_info_intf_ = hw_info_intf;
}

DisplayError HWHDMI::Init(HWEventHandler *eventhandler) {
  DisplayError error = kErrorNone;

  SetSourceProductInformation("vendor_name", "ro.product.manufacturer");
  SetSourceProductInformation("product_description", "ro.product.name");

  error = HWDevice::Init(eventhandler);
  if (error != kErrorNone) {
    return error;
  }

  error = ReadEDIDInfo();
  if (error != kErrorNone) {
    Deinit();
    return error;
  }

  if (!IsResolutionFilePresent()) {
    Deinit();
    return kErrorHardware;
  }

  // Mode look-up table for HDMI
  supported_video_modes_ = new msm_hdmi_mode_timing_info[hdmi_mode_count_];
  if (!supported_video_modes_) {
    Deinit();
    return kErrorMemory;
  }

  error = ReadTimingInfo();
  if (error != kErrorNone) {
    Deinit();
    return error;
  }

  ReadScanInfo();

  return error;
}

DisplayError HWHDMI::Deinit() {
  hdmi_mode_count_ = 0;
  if (supported_video_modes_) {
    delete[] supported_video_modes_;
  }

  return HWDevice::Deinit();
}

DisplayError HWHDMI::GetNumDisplayAttributes(uint32_t *count) {
  *count = hdmi_mode_count_;
  if (*count <= 0) {
    return kErrorHardware;
  }

  return kErrorNone;
}

DisplayError HWHDMI::GetActiveConfig(uint32_t *active_config_index) {
  *active_config_index = active_config_index_;
  return kErrorNone;
}

DisplayError HWHDMI::ReadEDIDInfo() {
  ssize_t length = -1;
  char edid_str[kPageSize] = {'\0'};
  char edid_path[kMaxStringLength] = {'\0'};
  snprintf(edid_path, sizeof(edid_path), "%s%d/edid_modes", fb_path_, fb_node_index_);
  int edid_file = Sys::open_(edid_path, O_RDONLY);
  if (edid_file < 0) {
    DLOGE("EDID file open failed.");
    return kErrorHardware;
  }

  length = Sys::pread_(edid_file, edid_str, sizeof(edid_str)-1, 0);
  if (length <= 0) {
    DLOGE("%s: edid_modes file empty");
    return kErrorHardware;
  }
  Sys::close_(edid_file);

  DLOGI("EDID mode string: %s", edid_str);
  while (length > 1 && isspace(edid_str[length-1])) {
    --length;
  }
  edid_str[length] = '\0';

  if (length > 0) {
    // Get EDID modes from the EDID string
    char *ptr = edid_str;
    const uint32_t edid_count_max = 128;
    char *tokens[edid_count_max] = { NULL };
    ParseLine(ptr, tokens, edid_count_max, &hdmi_mode_count_);
    for (uint32_t i = 0; i < hdmi_mode_count_; i++) {
      hdmi_modes_[i] = atoi(tokens[i]);
    }
  }

  return kErrorNone;
}

DisplayError HWHDMI::GetDisplayAttributes(uint32_t index,
                                          HWDisplayAttributes *display_attributes) {
  DTRACE_SCOPED();

  if (index > hdmi_mode_count_) {
    return kErrorNotSupported;
  }

  // Variable screen info
  STRUCT_VAR(fb_var_screeninfo, var_screeninfo);

  // Get the resolution info from the look up table
  msm_hdmi_mode_timing_info *timing_mode = &supported_video_modes_[0];
  for (uint32_t i = 0; i < hdmi_mode_count_; i++) {
    msm_hdmi_mode_timing_info *cur = &supported_video_modes_[i];
    if (cur->video_format == hdmi_modes_[index]) {
      timing_mode = cur;
      break;
    }
  }
  display_attributes->x_pixels = timing_mode->active_h;
  display_attributes->y_pixels = timing_mode->active_v;
  display_attributes->v_front_porch = timing_mode->front_porch_v;
  display_attributes->v_back_porch = timing_mode->back_porch_v;
  display_attributes->v_pulse_width = timing_mode->pulse_width_v;
  uint32_t h_blanking = timing_mode->front_porch_h + timing_mode->back_porch_h +
      timing_mode->pulse_width_h;
  display_attributes->h_total = timing_mode->active_h + h_blanking;
  display_attributes->x_dpi = 0;
  display_attributes->y_dpi = 0;
  display_attributes->fps = timing_mode->refresh_rate / 1000;
  display_attributes->vsync_period_ns = UINT32(1000000000L / display_attributes->fps);
  display_attributes->split_left = display_attributes->x_pixels;
  if (display_attributes->x_pixels > hw_resource_.max_mixer_width) {
    display_attributes->is_device_split = true;
    display_attributes->split_left = display_attributes->x_pixels / 2;
    display_attributes->h_total += h_blanking;
  }

  return kErrorNone;
}

DisplayError HWHDMI::SetDisplayAttributes(uint32_t index) {
  DTRACE_SCOPED();

  if (index > hdmi_mode_count_) {
    return kErrorNotSupported;
  }

  // Variable screen info
  STRUCT_VAR(fb_var_screeninfo, vscreeninfo);
  if (Sys::ioctl_(device_fd_, FBIOGET_VSCREENINFO, &vscreeninfo) < 0) {
    IOCTL_LOGE(FBIOGET_VSCREENINFO, device_type_);
    return kErrorHardware;
  }

  DLOGI("GetInfo<Mode=%d %dx%d (%d,%d,%d),(%d,%d,%d) %dMHz>", vscreeninfo.reserved[3],
        vscreeninfo.xres, vscreeninfo.yres, vscreeninfo.right_margin, vscreeninfo.hsync_len,
        vscreeninfo.left_margin, vscreeninfo.lower_margin, vscreeninfo.vsync_len,
        vscreeninfo.upper_margin, vscreeninfo.pixclock/1000000);

  msm_hdmi_mode_timing_info *timing_mode = &supported_video_modes_[0];
  for (uint32_t i = 0; i < hdmi_mode_count_; i++) {
    msm_hdmi_mode_timing_info *cur = &supported_video_modes_[i];
    if (cur->video_format == hdmi_modes_[index]) {
      timing_mode = cur;
      break;
    }
  }

  if (MapHDMIDisplayTiming(timing_mode, &vscreeninfo) == false) {
    return kErrorParameters;
  }

  STRUCT_VAR(msmfb_metadata, metadata);
  metadata.op = metadata_op_vic;
  metadata.data.video_info_code = timing_mode->video_format;
  if (Sys::ioctl_(device_fd_, MSMFB_METADATA_SET, &metadata) < 0) {
    IOCTL_LOGE(MSMFB_METADATA_SET, device_type_);
    return kErrorHardware;
  }

  DLOGI("SetInfo<Mode=%d %dx%d (%d,%d,%d),(%d,%d,%d) %dMHz>", vscreeninfo.reserved[3] & 0xFF00,
        vscreeninfo.xres, vscreeninfo.yres, vscreeninfo.right_margin, vscreeninfo.hsync_len,
        vscreeninfo.left_margin, vscreeninfo.lower_margin, vscreeninfo.vsync_len,
        vscreeninfo.upper_margin, vscreeninfo.pixclock/1000000);

  vscreeninfo.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_ALL | FB_ACTIVATE_FORCE;
  if (Sys::ioctl_(device_fd_, FBIOPUT_VSCREENINFO, &vscreeninfo) < 0) {
    IOCTL_LOGE(FBIOPUT_VSCREENINFO, device_type_);
    return kErrorHardware;
  }

  active_config_index_ = index;

  return kErrorNone;
}

DisplayError HWHDMI::GetConfigIndex(uint32_t mode, uint32_t *index) {
  // Check if the mode is valid and return corresponding index
  for (uint32_t i = 0; i < hdmi_mode_count_; i++) {
    if (hdmi_modes_[i] == mode) {
      *index = i;
      DLOGI("Index = %d for config = %d", *index, mode);
      return kErrorNone;
    }
  }

  DLOGE("Config = %d not supported", mode);
  return kErrorNotSupported;
}

DisplayError HWHDMI::Validate(HWLayers *hw_layers) {
  HWDevice::ResetDisplayParams();
  return HWDevice::Validate(hw_layers);
}

DisplayError HWHDMI::GetHWScanInfo(HWScanInfo *scan_info) {
  if (!scan_info) {
    return kErrorParameters;
  }
  *scan_info = hw_scan_info_;
  return kErrorNone;
}

DisplayError HWHDMI::GetVideoFormat(uint32_t config_index, uint32_t *video_format) {
  if (config_index > hdmi_mode_count_) {
    return kErrorNotSupported;
  }

  *video_format = hdmi_modes_[config_index];

  return kErrorNone;
}

DisplayError HWHDMI::GetMaxCEAFormat(uint32_t *max_cea_format) {
  *max_cea_format = HDMI_VFRMT_END;

  return kErrorNone;
}

DisplayError HWHDMI::OnMinHdcpEncryptionLevelChange(uint32_t min_enc_level) {
  DisplayError error = kErrorNone;
  int fd = -1;
  char data[kMaxStringLength] = {'\0'};

  snprintf(data, sizeof(data), "%s%d/hdcp2p2/min_level_change", fb_path_, fb_node_index_);

  fd = Sys::open_(data, O_WRONLY);
  if (fd < 0) {
    DLOGW("File '%s' could not be opened.", data);
    return kErrorHardware;
  }

  snprintf(data, sizeof(data), "%d", min_enc_level);

  ssize_t err = Sys::pwrite_(fd, data, strlen(data), 0);
  if (err <= 0) {
    DLOGE("Write failed, Error = %s", strerror(errno));
    error = kErrorHardware;
  }

  Sys::close_(fd);

  return error;
}

HWScanSupport HWHDMI::MapHWScanSupport(uint32_t value) {
  switch (value) {
  // TODO(user): Read the scan type from driver defined values instead of hardcoding
  case 0:
    return kScanNotSupported;
  case 1:
    return kScanAlwaysOverscanned;
  case 2:
    return kScanAlwaysUnderscanned;
  case 3:
    return kScanBoth;
  default:
    return kScanNotSupported;
    break;
  }
}

void HWHDMI::ReadScanInfo() {
  int scan_info_file = -1;
  ssize_t len = -1;
  char data[kPageSize] = {'\0'};

  snprintf(data, sizeof(data), "%s%d/scan_info", fb_path_, fb_node_index_);
  scan_info_file = Sys::open_(data, O_RDONLY);
  if (scan_info_file < 0) {
    DLOGW("File '%s' not found.", data);
    return;
  }

  memset(&data[0], 0, sizeof(data));
  len = Sys::pread_(scan_info_file, data, sizeof(data) - 1, 0);
  if (len <= 0) {
    Sys::close_(scan_info_file);
    DLOGW("File %s%d/scan_info is empty.", fb_path_, fb_node_index_);
    return;
  }
  data[len] = '\0';
  Sys::close_(scan_info_file);

  const uint32_t scan_info_max_count = 3;
  uint32_t scan_info_count = 0;
  char *tokens[scan_info_max_count] = { NULL };
  ParseLine(data, tokens, scan_info_max_count, &scan_info_count);
  if (scan_info_count != scan_info_max_count) {
    DLOGW("Failed to parse scan info string %s", data);
    return;
  }

  hw_scan_info_.pt_scan_support = MapHWScanSupport(atoi(tokens[0]));
  hw_scan_info_.it_scan_support = MapHWScanSupport(atoi(tokens[1]));
  hw_scan_info_.cea_scan_support = MapHWScanSupport(atoi(tokens[2]));
  DLOGI("PT %d IT %d CEA %d", hw_scan_info_.pt_scan_support, hw_scan_info_.it_scan_support,
        hw_scan_info_.cea_scan_support);
}

int HWHDMI::OpenResolutionFile(int file_mode) {
  char file_path[kMaxStringLength];
  memset(file_path, 0, sizeof(file_path));
  snprintf(file_path , sizeof(file_path), "%s%d/res_info", fb_path_, fb_node_index_);

  int fd = Sys::open_(file_path, file_mode);

  if (fd < 0) {
    DLOGE("file '%s' not found : ret = %d err str: %s", file_path, fd, strerror(errno));
  }

  return fd;
}

// Method to request HDMI driver to write a new page of timing info into res_info node
void HWHDMI::RequestNewPage(uint32_t page_number) {
  char page_string[kPageSize];
  int fd = OpenResolutionFile(O_WRONLY);
  if (fd < 0) {
    return;
  }

  snprintf(page_string, sizeof(page_string), "%d", page_number);

  DLOGI_IF(kTagDriverConfig, "page=%s", page_string);

  ssize_t err = Sys::pwrite_(fd, page_string, sizeof(page_string), 0);
  if (err <= 0) {
    DLOGE("Write to res_info failed (%s)", strerror(errno));
  }

  Sys::close_(fd);
}

// Reads the contents of res_info node into a buffer if the file is not empty
bool HWHDMI::ReadResolutionFile(char *config_buffer) {
  ssize_t bytes_read = 0;
  int fd = OpenResolutionFile(O_RDONLY);
  if (fd >= 0) {
    bytes_read = Sys::pread_(fd, config_buffer, kPageSize, 0);
    Sys::close_(fd);
  }

  DLOGI_IF(kTagDriverConfig, "bytes_read = %d", bytes_read);

  return (bytes_read > 0);
}

// Populates the internal timing info structure with the timing info obtained
// from the HDMI driver
DisplayError HWHDMI::ReadTimingInfo() {
  uint32_t config_index = 0;
  uint32_t page_number = MSM_HDMI_INIT_RES_PAGE;
  uint32_t size = sizeof(msm_hdmi_mode_timing_info);

  while (true) {
    char config_buffer[kPageSize] = {0};
    msm_hdmi_mode_timing_info *info = reinterpret_cast<msm_hdmi_mode_timing_info *>(config_buffer);
    RequestNewPage(page_number);

    if (!ReadResolutionFile(config_buffer)) {
      break;
    }

    while (info->video_format && size < kPageSize && config_index < hdmi_mode_count_) {
      supported_video_modes_[config_index] = *info;
      size += sizeof(msm_hdmi_mode_timing_info);

      DLOGI_IF(kTagDriverConfig, "Config=%d Mode %d: (%dx%d) @ %d, pixel formats %d",
               config_index,
               supported_video_modes_[config_index].video_format,
               supported_video_modes_[config_index].active_h,
               supported_video_modes_[config_index].active_v,
               supported_video_modes_[config_index].refresh_rate,
               supported_video_modes_[config_index].pixel_formats);

      info++;
      config_index++;
    }

    size = sizeof(msm_hdmi_mode_timing_info);
    // Request HDMI driver to populate res_info with more
    // timing information
    page_number++;
  }

  if (page_number == MSM_HDMI_INIT_RES_PAGE || config_index == 0) {
    DLOGE("No timing information found.");
    return kErrorHardware;
  }

  return kErrorNone;
}

bool HWHDMI::IsResolutionFilePresent() {
  bool is_file_present = false;
  int fd = OpenResolutionFile(O_RDONLY);
  if (fd >= 0) {
    is_file_present = true;
    Sys::close_(fd);
  }

  return is_file_present;
}

void HWHDMI::SetSourceProductInformation(const char *node, const char *name) {
  char property_value[kMaxStringLength];
  char sys_fs_path[kMaxStringLength];
  int hdmi_node_index = GetFBNodeIndex(kDeviceHDMI);
  if (hdmi_node_index < 0) {
    return;
  }

  ssize_t length = 0;
  bool prop_read_success = Debug::GetProperty(name, property_value);
  if (!prop_read_success) {
    return;
  }

  snprintf(sys_fs_path , sizeof(sys_fs_path), "%s%d/%s", fb_path_, hdmi_node_index, node);
  length = HWDevice::SysFsWrite(sys_fs_path, property_value, strlen(property_value));
  if (length <= 0) {
    DLOGW("Failed to write %s = %s", node, property_value);
  }
}

}  // namespace sdm

