/**************************************************************************************************
*
* Copyright (c) 2019-2025 Axera Semiconductor Co., Ltd. All Rights Reserved.
*
* This source file is the property of Axera Semiconductor Co., Ltd. and
* may not be copied or distributed in any isomorphic form without the prior
* written consent of Axera Semiconductor Co., Ltd.
*
*************************************************************************************************/

#ifndef _USB_UPDATA_H_
#define _USB_UPDATA_H_

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef enum {
    FDL_CMD_TYPE_MIN            = 0,
    FDL_CMD_CONNECT             = FDL_CMD_TYPE_MIN,
    FDL_CMD_START_TRANSFER      = 1,
    FDL_CMD_START_TRANSFER_DATA = 2,
    FDL_CMD_START_TRANSFER_END  = 3,
    FDL_CMD_EXECUTE             = 4,
    FDL_CMD_HANDSHAKE           = 5,
    FDL_CMD_ENABLE_QUAD         = 7,
    FDL_CMD_UPDATE_FREQ         = 8,
    FDL_CMD_SLV_FREQ_BOOST      = 9,
    FDL_CMD_TYPE_MAX,

    FDL_RESP_TYPE_MIN             = 0x80,
    FDL_RESP_ACK                  = FDL_RESP_TYPE_MIN,
    FDL_RESP_VERSION              = 0x81,
    FDL_RESP_INVLID_CMD           = 0x82,
    FDL_RESP_UNKNOWN_CMD          = 0x83,
    FDL_RESP_OPERATION_FAIL       = 0x84,
    FDL_EIPFW_INVALID             = 0x85,
    FDL_EIPFW_DATA_ERR            = 0x86,
    FDL_EIPFW_HASH_ERR            = 0x87,
    FDL_RESP_SECURE_SIGNATURE_ERR = 0x88,
    FDL_RESP_DEST_ERR             = 0x89,
    FDL_RESP_SIZE_ERR             = 0x8A,
    FDL_RESP_VERIFY_ERROR         = 0x8B,
    FDL_RESP_IMG_HEADER_ERROR     = 0x8C,

    // 擦写控制，通知NHBurner工具配合
    FDL_RESP_UPDATE_IN = 0x92,

    FDL_RESP_TYPE_MAX
} CMD_TYPE;

#define UVC_VENDOR_ID  0x1d6b  // 同uvc设备一样
#define UVC_PRODUCT_ID 0x0102  // 同uvc设备一样

#define VENDOR_ID  0x32c9  // 同mboot udc一样
#define PRODUCT_ID 0x1000  // 同mboot Product ID一样

/* 端点2发送数据，端点1接收数据 */
#define USB_UPDATE_OUT_ENDPOINT 0x2
#define USB_UPDATE_IN_ENDPOINT  0x81

#define TIMEOUT_S       1000         // 超时单位s
#define BUFFER_SIZE     (1024 + 12)  // 数据包缓存长度
#define MAX_ERRORS      (2)          // 错误重传次数
#define UPDATE_PER_PACK 0x100        // 单次数据包长度, 不大于(BUFFER_SIZE - 12)

#define SYS_USB_PATH "/sys/bus/usb/devices"
extern int libusb_get_vid_pid(void);
extern int libusb_get_vid_pid_with_callback(void (*on_wait)(void *), void *context);

#define up_debug(fmt, ...)  // printf(fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif
