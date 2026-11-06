/* Single-source definitions: ALVR server settings. */
#ifdef HAVE_ALVR
S_BOOL(alvr_enable, ALVR_ENABLE,
      "alvr_enable", false, SD_FLAG_NONE, 0, 0,
      "Enable ALVR Server",
      "Stream to the ALVR headset at the configured address. Disable before changing stream settings.")
#ifndef SETTINGS_DEF_CONFIG_PASS
S_STRING(alvr_client_address, ALVR_CLIENT_ADDRESS,
      "alvr_client_address", "", SD_FLAG_ALLOW_INPUT, 0, NULL, NULL,
      setting_generic_action_start_default, NULL, NULL, NULL, ST_UI_TYPE_STRING_LINE_EDIT,
      "Headset Address",
      "IPv4 address of the headset running ALVR.")
#endif
S_UINT(alvr_view_size, ALVR_VIEW_SIZE,
      "alvr_view_size", 0, SD_FLAG_NONE, SDESC_RANGE_MINMAX, 0,
      0, 4096, 32, 0, setting_action_ok_uint, NULL,
      "Resolution Width per Eye",
      "0: 100% of the headset-recommended resolution. Otherwise, set the width per eye; height follows the headset aspect ratio. Dimensions are rounded down to multiples of 32.")
S_UINT(alvr_bitrate, ALVR_BITRATE,
      "alvr_bitrate", 150, SD_FLAG_NONE, SDESC_RANGE_MINMAX, 0,
      1, 150, 1, 0, setting_action_ok_uint, NULL,
      "Video Bitrate (Mbps)",
      "Target bitrate for streamed video.")
S_UINT(alvr_codec, ALVR_CODEC,
      "alvr_codec", 0, SD_FLAG_NONE, SDESC_RANGE_MINMAX, 0,
      0, 2, 1, 0, setting_action_ok_uint, setting_get_string_representation_alvr_codec,
      "Video Codec",
      "Automatic prefers H.265 and falls back to H.264 when the encoder is unavailable. Disable ALVR before changing this setting.")
S_UINT(alvr_encoder, ALVR_ENCODER,
      "alvr_encoder", 0, SD_FLAG_NONE, SDESC_RANGE_MINMAX, 0,
      0, 2, 1, 0, setting_action_ok_uint, NULL,
      "Video Encoder",
      "0: Automatic. 1: Vulkan Video. 2: Media Foundation (Windows).")
#endif
