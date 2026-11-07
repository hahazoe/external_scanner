/*
 * UAC1 耳机(Headset)描述符模板：立体声扬声器 + 单声道麦克风
 * 基于 TinyUSB uac2_headset 例程裁剪（ESP32-S3 仅全速，走 UAC1）
 */
#ifndef USB_DESCRIPTORS_H_
#define USB_DESCRIPTORS_H_

enum
{
  ITF_NUM_AUDIO_CONTROL = 0,
  ITF_NUM_AUDIO_STREAMING_SPK,
  ITF_NUM_AUDIO_STREAMING_MIC,
  ITF_NUM_TOTAL
};

//--------------------------------------------------------------------+
// UAC1 DESCRIPTOR TEMPLATES
//--------------------------------------------------------------------+
// 扬声器路径实体 ID
#define UAC1_ENTITY_SPK_INPUT_TERMINAL  0x01
#define UAC1_ENTITY_SPK_FEATURE_UNIT    0x02
#define UAC1_ENTITY_SPK_OUTPUT_TERMINAL 0x03
// 麦克风路径实体 ID
#define UAC1_ENTITY_MIC_INPUT_TERMINAL  0x11
#define UAC1_ENTITY_MIC_OUTPUT_TERMINAL 0x13

// 配置描述符总长度（不含 TUD_CONFIG_DESC_LEN），_nfreqs = 采样率个数
#define TUD_AUDIO10_HEADSET_STEREO_DESC_LEN(_nfreqs) (\
    +TUD_AUDIO10_DESC_STD_AC_LEN\
    + TUD_AUDIO10_DESC_CS_AC_LEN(2)\
    + TUD_AUDIO10_DESC_INPUT_TERM_LEN\
    + TUD_AUDIO10_DESC_FEATURE_UNIT_LEN(2)\
    + TUD_AUDIO10_DESC_OUTPUT_TERM_LEN\
    + TUD_AUDIO10_DESC_INPUT_TERM_LEN\
    + TUD_AUDIO10_DESC_OUTPUT_TERM_LEN\
    /* Interface 1, Alternate 0 (speaker) */\
    + TUD_AUDIO10_DESC_STD_AS_LEN\
    /* Interface 1, Alternate 1 (speaker) */\
    + TUD_AUDIO10_DESC_STD_AS_LEN\
    + TUD_AUDIO10_DESC_CS_AS_INT_LEN\
    + TUD_AUDIO10_DESC_TYPE_I_FORMAT_LEN(_nfreqs)\
    + TUD_AUDIO10_DESC_STD_AS_ISO_EP_LEN\
    + TUD_AUDIO10_DESC_CS_AS_ISO_EP_LEN\
    /* Interface 2, Alternate 0 (microphone) */\
    + TUD_AUDIO10_DESC_STD_AS_LEN\
    /* Interface 2, Alternate 1 (microphone) */\
    + TUD_AUDIO10_DESC_STD_AS_LEN\
    + TUD_AUDIO10_DESC_CS_AS_INT_LEN\
    + TUD_AUDIO10_DESC_TYPE_I_FORMAT_LEN(_nfreqs)\
    + TUD_AUDIO10_DESC_STD_AS_ISO_EP_LEN\
    + TUD_AUDIO10_DESC_CS_AS_ISO_EP_LEN)

// 注意：CS_AC 的 wTotalLength 必须等于实际发出的 AC 类描述符字节数。
// 上游例程在此多算了一个不存在的麦克风 Feature Unit，本工程已修正。
#define TUD_AUDIO10_HEADSET_STEREO_DESCRIPTOR(_itfnum, _stridx, _nBytesPerSample_RX, _nBitsUsedPerSample_RX, _nBytesPerSample_TX, _nBitsUsedPerSample_TX, _epout, _epoutsize, _epin, _epinsize, ...) \
    /* Standard AC Interface Descriptor(4.3.1) */\
    TUD_AUDIO10_DESC_STD_AC(/*_itfnum*/ _itfnum, /*_nEPs*/ 0x00, /*_stridx*/ _stridx),\
    /* Class-Specific AC Interface Header Descriptor(4.3.2) */\
    TUD_AUDIO10_DESC_CS_AC(/*_bcdADC*/ 0x0100, /*_totallen*/ (TUD_AUDIO10_DESC_INPUT_TERM_LEN+TUD_AUDIO10_DESC_FEATURE_UNIT_LEN(2)+TUD_AUDIO10_DESC_OUTPUT_TERM_LEN+TUD_AUDIO10_DESC_INPUT_TERM_LEN+TUD_AUDIO10_DESC_OUTPUT_TERM_LEN), /*_itf*/ ((_itfnum)+1), ((_itfnum)+2)),\
    /* Speaker Input Terminal Descriptor(4.3.2.1) */\
    TUD_AUDIO10_DESC_INPUT_TERM(/*_termid*/ UAC1_ENTITY_SPK_INPUT_TERMINAL, /*_termtype*/ AUDIO_TERM_TYPE_USB_STREAMING, /*_assocTerm*/ UAC1_ENTITY_MIC_OUTPUT_TERMINAL, /*_nchannels*/ 0x02, /*_channelcfg*/ AUDIO10_CHANNEL_CONFIG_NON_PREDEFINED, /*_idxchannelnames*/ 0x00, /*_stridx*/ 0x00),\
    /* Speaker Feature Unit Descriptor(4.3.2.5) */\
    TUD_AUDIO10_DESC_FEATURE_UNIT(/*_unitid*/ UAC1_ENTITY_SPK_FEATURE_UNIT, /*_srcid*/ UAC1_ENTITY_SPK_INPUT_TERMINAL, /*_stridx*/ 0x00, /*_ctrlmaster*/ (AUDIO10_FU_CONTROL_BM_MUTE | AUDIO10_FU_CONTROL_BM_VOLUME), /*_ctrlch1*/ (AUDIO10_FU_CONTROL_BM_MUTE | AUDIO10_FU_CONTROL_BM_VOLUME), /*_ctrlch2*/ (AUDIO10_FU_CONTROL_BM_MUTE | AUDIO10_FU_CONTROL_BM_VOLUME)),\
    /* Speaker Output Terminal Descriptor(4.3.2.2) */\
    TUD_AUDIO10_DESC_OUTPUT_TERM(/*_termid*/ UAC1_ENTITY_SPK_OUTPUT_TERMINAL, /*_termtype*/ AUDIO_TERM_TYPE_OUT_HEADPHONES, /*_assocTerm*/ 0x00, /*_srcid*/ UAC1_ENTITY_SPK_FEATURE_UNIT, /*_stridx*/ 0x00),\
    /* Microphone Input Terminal Descriptor(4.3.2.1) */\
    TUD_AUDIO10_DESC_INPUT_TERM(/*_termid*/ UAC1_ENTITY_MIC_INPUT_TERMINAL, /*_termtype*/ AUDIO_TERM_TYPE_IN_GENERIC_MIC, /*_assocTerm*/ 0x00, /*_nchannels*/ 0x01, /*_channelcfg*/ AUDIO10_CHANNEL_CONFIG_NON_PREDEFINED, /*_idxchannelnames*/ 0x00, /*_stridx*/ 0x00),\
    /* Microphone Output Terminal Descriptor(4.3.2.2) */\
    TUD_AUDIO10_DESC_OUTPUT_TERM(/*_termid*/ UAC1_ENTITY_MIC_OUTPUT_TERMINAL, /*_termtype*/ AUDIO_TERM_TYPE_USB_STREAMING, /*_assocTerm*/ UAC1_ENTITY_SPK_INPUT_TERMINAL, /*_srcid*/ UAC1_ENTITY_MIC_INPUT_TERMINAL, /*_stridx*/ 0x00),\
    /* Standard AS Interface Descriptor(4.5.1) - Speaker Interface 1, Alternate 0 */\
    TUD_AUDIO10_DESC_STD_AS_INT(/*_itfnum*/ (uint8_t)((_itfnum)+1), /*_altset*/ 0x00, /*_nEPs*/ 0x00, /*_stridx*/ _stridx),\
    /* Standard AS Interface Descriptor(4.5.1) - Speaker Interface 1, Alternate 1 */\
    TUD_AUDIO10_DESC_STD_AS_INT(/*_itfnum*/ (uint8_t)((_itfnum)+1), /*_altset*/ 0x01, /*_nEPs*/ 0x01, /*_stridx*/ _stridx),\
    /* Class-Specific AS Interface Descriptor(4.5.2) */\
    TUD_AUDIO10_DESC_CS_AS_INT(/*_termid*/ UAC1_ENTITY_SPK_INPUT_TERMINAL, /*_delay*/ 0x01, /*_formattype*/ AUDIO10_DATA_FORMAT_TYPE_I_PCM),\
    /* Type I Format Type Descriptor(2.2.5) */\
    TUD_AUDIO10_DESC_TYPE_I_FORMAT(/*_nrchannels*/ 0x02, /*_subframesize*/ _nBytesPerSample_RX, /*_bitresolution*/ _nBitsUsedPerSample_RX, /*_freqs*/ __VA_ARGS__),\
    /* Standard AS Isochronous Audio Data Endpoint Descriptor(4.6.1.1) */\
    TUD_AUDIO10_DESC_STD_AS_ISO_EP(/*_ep*/ _epout, /*_attr*/ (uint8_t) ((uint8_t)TUSB_XFER_ISOCHRONOUS | (uint8_t)TUSB_ISO_EP_ATT_ADAPTIVE), /*_maxEPsize*/ _epoutsize, /*_interval*/ 0x01, /*_syncep*/ 0x00),\
    /* Class-Specific AS Isochronous Audio Data Endpoint Descriptor(4.6.1.2) */\
    TUD_AUDIO10_DESC_CS_AS_ISO_EP(/*_attr*/ AUDIO10_CS_AS_ISO_DATA_EP_ATT_SAMPLING_FRQ, /*_lockdelayunits*/ AUDIO10_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_MILLISEC, /*_lockdelay*/ 0x0001),\
    /* Standard AS Interface Descriptor(4.5.1) - Microphone Interface 2, Alternate 0 */\
    TUD_AUDIO10_DESC_STD_AS_INT(/*_itfnum*/ (uint8_t)((_itfnum)+2), /*_altset*/ 0x00, /*_nEPs*/ 0x00, /*_stridx*/ _stridx),\
    /* Standard AS Interface Descriptor(4.5.1) - Microphone Interface 2, Alternate 1 */\
    TUD_AUDIO10_DESC_STD_AS_INT(/*_itfnum*/ (uint8_t)((_itfnum)+2), /*_altset*/ 0x01, /*_nEPs*/ 0x01, /*_stridx*/ _stridx),\
    /* Class-Specific AS Interface Descriptor(4.5.2) */\
    TUD_AUDIO10_DESC_CS_AS_INT(/*_termid*/ UAC1_ENTITY_MIC_OUTPUT_TERMINAL, /*_delay*/ 0x01, /*_formattype*/ AUDIO10_DATA_FORMAT_TYPE_I_PCM),\
    /* Type I Format Type Descriptor(2.2.5) */\
    TUD_AUDIO10_DESC_TYPE_I_FORMAT(/*_nrchannels*/ 0x01, /*_subframesize*/ _nBytesPerSample_TX, /*_bitresolution*/ _nBitsUsedPerSample_TX, /*_freqs*/ __VA_ARGS__),\
    /* Standard AS Isochronous Audio Data Endpoint Descriptor(4.6.1.1) */\
    TUD_AUDIO10_DESC_STD_AS_ISO_EP(/*_ep*/ _epin, /*_attr*/ (uint8_t) ((uint8_t)TUSB_XFER_ISOCHRONOUS | (uint8_t)TUSB_ISO_EP_ATT_ASYNCHRONOUS), /*_maxEPsize*/ _epinsize, /*_interval*/ 0x01, /*_syncep*/ 0x00),\
    /* Class-Specific AS Isochronous Audio Data Endpoint Descriptor(4.6.1.2) */\
    TUD_AUDIO10_DESC_CS_AS_ISO_EP(/*_attr*/ AUDIO10_CS_AS_ISO_DATA_EP_ATT_SAMPLING_FRQ, /*_lockdelayunits*/ AUDIO10_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_MILLISEC, /*_lockdelay*/ 0x0001)

#endif
