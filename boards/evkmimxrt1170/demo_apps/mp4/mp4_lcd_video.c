/*
 * Copyright 2019 NXP
 * All rights reserved.
 *
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <string.h>
#include "board.h"
#include "pin_mux.h"
#include "clock_config.h"
#include "fsl_common.h"
#include "fsl_debug_console.h"
#include "fsl_pxp.h"
#include "fsl_lcdifv2.h"
#include "display_support.h"
#include "fsl_cache.h"
#include "fsl_gpio.h"
#include "mp4.h"
/*******************************************************************************
 * Definitions
 ******************************************************************************/
#define APP_ELCDIF LCDIFV2
#define APP_ELCDIF_LAYER 0
#define APP_CORE_ID 0

#if VIDEO_LCD_RESOLUTION_SVGA540 == 1
#define APP_IMG_HEIGHT 960
#define APP_IMG_WIDTH 540
#define APP_IMG_MAX_SIDE_LEN 960
#define APP_HSW 2
#define APP_HFP 32
#define APP_HBP 30
#define APP_VSW 2
#define APP_VFP 16
#define APP_VBP 14
#elif VIDEO_LCD_RESOLUTION_WXGA720 == 1
#define APP_IMG_HEIGHT 1280
#define APP_IMG_WIDTH 720
#define APP_IMG_MAX_SIDE_LEN 1280
#define APP_HSW 8
#define APP_HFP 32
#define APP_HBP 32
#define APP_VSW 2
#define APP_VFP 16
#define APP_VBP 14
#endif

#define APP_POL_FLAGS \
    (kLCDIFV2_DataEnableActiveHigh | kLCDIFV2_VsyncActiveLow | kLCDIFV2_HsyncActiveLow | kLCDIFV2_DriveDataOnRisingClkEdge)

#define APP_LCDIF_DATA_BUS kELCDIF_DataBus16Bit

/* Frame buffer data alignment, for better performance, the LCDIF frame buffer should be 64B align. */
#define FRAME_BUFFER_ALIGN 64
/*
 * For better performance, three frame buffers are used in this demo.
 */
#define APP_LCD_FB_NUM 2 /* LCD frame buffer number. */

/* Cache line size. */
#ifndef FSL_FEATURE_L2CACHE_LINESIZE_BYTE
#define FSL_FEATURE_L2CACHE_LINESIZE_BYTE 0
#endif
#ifndef FSL_FEATURE_L1DCACHE_LINESIZE_BYTE
#define FSL_FEATURE_L1DCACHE_LINESIZE_BYTE 0
#endif

#if (FSL_FEATURE_L2CACHE_LINESIZE_BYTE > FSL_FEATURE_L1DCACHE_LINESIZE_BYTE)
#define APP_CACHE_LINE_SIZE FSL_FEATURE_L2CACHE_LINESIZE_BYTE
#else
#define APP_CACHE_LINE_SIZE FSL_FEATURE_L1DCACHE_LINESIZE_BYTE
#endif

#define APP_PXP PXP
#define APP_PS_WIDTH  1280 /* 1280,800,image resolution*/
#define APP_PS_HEIGHT 800  /* 1280,800,image resolution*/

#if VIDEO_PIXEL_FMT_RGB888 == 1
#define APP_BPP 4U
#elif VIDEO_PIXEL_FMT_RGB565 == 1
#define APP_BPP 2U
#endif

#define APP_PS_ULC_X 0U
#define APP_PS_ULC_Y 0U

/*******************************************************************************
 * Prototypes
 ******************************************************************************/
void BOARD_EnableLcdInterrupt(void);
void BOARD_InitLcdifClock(void);

/*******************************************************************************
 * Variables
 ******************************************************************************/
video_lcd_cfg_t g_videoLcdCfg;

static volatile bool g_lcdFramePending = false;
static void *volatile s_fbList = NULL; /* List to the frame buffers. */
static void *volatile inactiveBuf = NULL;
static void *volatile activeBuf = NULL;

static pxp_output_buffer_config_t outputBufferConfig;
static pxp_ps_buffer_config_t psBufferConfig;
#if VIDEO_PXP_CONV_BLOCKING == 0
AT_NONCACHEABLE_SECTION(static uint8_t s_convBufferYUV[3][APP_PS_HEIGHT][APP_PS_WIDTH]);
#endif
AT_NONCACHEABLE_SECTION_ALIGN(static uint8_t s_psBufferLcd[APP_LCD_FB_NUM][APP_IMG_MAX_SIDE_LEN][APP_IMG_MAX_SIDE_LEN][APP_BPP], FRAME_BUFFER_ALIGN);

/*******************************************************************************
 * Code
 ******************************************************************************/

/* Enable interrupt. */
void BOARD_EnableLcdInterrupt(void)
{
    EnableIRQ(LCDIF2_IRQn);
}

void BOARD_InitLcdifPixelClock(void)
{
    BOARD_InitLcdifClock();
}

/* Put the unused frame buffer to the s_fbList. */
static void APP_PutFrameBuffer(void *fb)
{
    *(void **)fb = s_fbList;
    s_fbList = fb;
}

static void APP_InitPxp(uint32_t psWidth)
{
    PXP_Init(APP_PXP);

    /* PS configure. */
    psBufferConfig.pixelFormat = kPXP_PsPixelFormatYVU420;
    psBufferConfig.swapByte = false;
    psBufferConfig.bufferAddr = 0U;
    psBufferConfig.bufferAddrU = 0U;
    psBufferConfig.bufferAddrV = 0U;
    psBufferConfig.pitchBytes = psWidth;
    PXP_SetProcessSurfaceBackGroundColor(APP_PXP, 0U);
    PXP_SetProcessSurfaceBufferConfig(APP_PXP, &psBufferConfig);
    /* Disable AS. */
    PXP_SetAlphaSurfacePosition(APP_PXP, 0xFFFFU, 0xFFFFU, 0U, 0U);

    /* Output config. */
#if VIDEO_PIXEL_FMT_RGB888 == 1
    outputBufferConfig.pixelFormat = kPXP_OutputPixelFormatRGB888;
#elif VIDEO_PIXEL_FMT_RGB565 == 1
    outputBufferConfig.pixelFormat = kPXP_OutputPixelFormatRGB565;
#endif
    outputBufferConfig.interlacedMode = kPXP_OutputProgressive;
    outputBufferConfig.buffer0Addr = (uint32_t)s_psBufferLcd[0];
    outputBufferConfig.buffer1Addr = 0U;
    outputBufferConfig.pitchBytes = APP_IMG_WIDTH * APP_BPP;
    outputBufferConfig.width = APP_IMG_WIDTH;
    outputBufferConfig.height = APP_IMG_HEIGHT;
    PXP_SetOutputBufferConfig(APP_PXP, &outputBufferConfig);

    /* Disable CSC1, it is enabled by default. */
    PXP_SetCsc1Mode(APP_PXP, kPXP_Csc1YCbCr2RGB);
    PXP_EnableCsc1(APP_PXP, true);
}

static void APP_InitLcdif(void)
{   
    const lcdifv2_display_config_t lcdifv2Config = {
        .panelWidth = APP_IMG_WIDTH,
        .panelHeight = APP_IMG_HEIGHT,
        .hsw = APP_HSW,
        .hfp = APP_HFP,
        .hbp = APP_HBP,
        .vsw = APP_VSW,
        .vfp = APP_VFP,
        .vbp = APP_VBP,
        .polarityFlags = APP_POL_FLAGS,
        .lineOrder     = kLCDIFV2_LineOrderRGB,
    };

    const lcdifv2_buffer_config_t fbConfig = {
        .strideBytes = APP_IMG_WIDTH * APP_BPP,
#if VIDEO_PIXEL_FMT_RGB888 == 1
        .pixelFormat = kLCDIFV2_PixelFormatARGB8888,
#elif VIDEO_PIXEL_FMT_RGB565 == 1
        .pixelFormat = kLCDIFV2_PixelFormatRGB565,
#endif
    };

    if (kStatus_Success != BOARD_InitDisplayInterface())
    {
        PRINTF("Display interface initialize failed\r\n");

        while (1)
        {
        }
    }

    for (uint32_t i = 1; i < APP_LCD_FB_NUM; i++)
    {
        APP_PutFrameBuffer(s_psBufferLcd[i]);
    }
    activeBuf = s_psBufferLcd[0];

    LCDIFV2_Init(APP_ELCDIF);
    LCDIFV2_SetDisplayConfig(APP_ELCDIF, &lcdifv2Config);
    LCDIFV2_EnableDisplay(APP_ELCDIF, true);
    LCDIFV2_SetLayerBufferConfig(APP_ELCDIF, APP_ELCDIF_LAYER, &fbConfig);
    LCDIFV2_SetLayerSize(APP_ELCDIF, APP_ELCDIF_LAYER, APP_IMG_WIDTH, APP_IMG_HEIGHT);
    LCDIFV2_SetLayerOffset(APP_ELCDIF, APP_ELCDIF_LAYER, 0, 0);

    LCDIFV2_EnableLayer(APP_ELCDIF, APP_ELCDIF_LAYER, true);
}

#if MP4_LCD_TIME_ENABLE == 1
extern void time_measure_start(void);
extern uint64_t time_measure_done(void);
lcd_measure_context_t g_lcdMeasureContext;
#endif //#if MP4_LCD_TIME_ENABLE

typedef enum _lcd_time_type
{
    kLcdTimeType_Start = 0U,
    kLcdTimeType_Pxp   = 1U,
    kLcdTimeType_Lcd   = 2U,
} lcd_time_type_t;

static void lcd_time_measure_utility(lcd_time_type_t type)
{
#if MP4_LCD_TIME_ENABLE == 1
    if (type == kLcdTimeType_Start)
    {
        time_measure_start();
    }
    else if (type == kLcdTimeType_Pxp)
    {
        g_lcdMeasureContext.costTimePxp_ns = time_measure_done();
    }
    else if (type == kLcdTimeType_Lcd)
    {
        g_lcdMeasureContext.costTimeLcd_ns = time_measure_done();
    }
#else
    // Do nothing
#endif
}

void lcd_video_display(uint8_t *buf[], uint32_t xsize, uint32_t ysize)
{
    static uint8_t curLcdBufferIdx = 1U;
    static bool isPxpFirstStart = true;
#if VIDEO_LCD_DISP_BLOCKING == 0
    static bool isLcdCurFrameDone = true;

#if VIDEO_PXP_CONV_BLOCKING == 0
    if (!isPxpFirstStart)
    {
#if VIDEO_PXP_CONV_WAITING == 1
        lcd_time_measure_utility(kLcdTimeType_Start);
        // Wait util PXP complete, As it is not first frame
        while (!(kPXP_CompleteFlag & PXP_GetStatusFlags(APP_PXP)))
        {
        }
        lcd_time_measure_utility(kLcdTimeType_Pxp);
#endif // #if VIDEO_PXP_CONV_WAITING == 1
        PXP_ClearStatusFlags(APP_PXP, kPXP_CompleteFlag);
#endif // #if VIDEO_PXP_CONV_BLOCKING == 0

        if (!isLcdCurFrameDone)
        {
#if VIDEO_LCD_DISP_WAITING == 1
            lcd_time_measure_utility(kLcdTimeType_Start);
            // Wait util last LCD frame done, then we can show current frame
            while (!(kLCDIFV2_VerticalBlankingInterrupt & LCDIFV2_GetInterruptStatus(APP_ELCDIF, APP_CORE_ID)))
            {
            }
            lcd_time_measure_utility(kLcdTimeType_Lcd);
#endif // #if VIDEO_LCD_DISP_WAITING == 1
        }
        else
        {
            isLcdCurFrameDone = false;
        }

#if VIDEO_PXP_CONV_BLOCKING == 0
        // Start to show current frame via LCD
        LCDIFV2_SetLayerBufferAddr(APP_ELCDIF, APP_ELCDIF_LAYER, (uint32_t)s_psBufferLcd[curLcdBufferIdx]);
        LCDIFV2_TriggerLayerShadowLoad(APP_ELCDIF, APP_ELCDIF_LAYER);
        LCDIFV2_ClearInterruptStatus(APP_ELCDIF, APP_CORE_ID, kLCDIFV2_VerticalBlankingInterrupt);

        curLcdBufferIdx++;
        curLcdBufferIdx %= APP_LCD_FB_NUM;
    }
    else
    {
        isPxpFirstStart = false;
    }

    // Copy over frame YUV data, so we can use ping-pong frame buffer
    for (uint32_t i = 0; i < 3; i++)
    {
        memcpy(s_convBufferYUV[i], buf[i], xsize * ysize);
    }
    psBufferConfig.bufferAddr = (uint32_t)s_convBufferYUV[0];
    psBufferConfig.bufferAddrU = (uint32_t)s_convBufferYUV[1];
    psBufferConfig.bufferAddrV = (uint32_t)s_convBufferYUV[2];
#else // #if VIDEO_PXP_CONV_BLOCKING == 1
    psBufferConfig.bufferAddr = (uint32_t)buf[0];
    psBufferConfig.bufferAddrU = (uint32_t)buf[1];
    psBufferConfig.bufferAddrV = (uint32_t)buf[2];
#endif // #if VIDEO_PXP_CONV_BLOCKING == 0
#else  // #if VIDEO_LCD_DISP_BLOCKING == 1
#if VIDEO_PXP_CONV_BLOCKING == 1
    psBufferConfig.bufferAddr = (uint32_t)buf[0];
    psBufferConfig.bufferAddrU = (uint32_t)buf[1];
    psBufferConfig.bufferAddrV = (uint32_t)buf[2];
#else // #if VIDEO_PXP_CONV_BLOCKING == 0
#error "Unsupported PXP_CONV_BLOCKING=0, LCD_DISP_BLOCKING=1 configuration case"
#endif // #if VIDEO_PXP_CONV_BLOCKING == 1
#endif // #if VIDEO_LCD_DISP_BLOCKING == 0

    // Start to convert next frame via PXP
    PXP_SetProcessSurfaceBufferConfig(APP_PXP, &psBufferConfig);
    //PXP_SetProcessSurfaceScaler(APP_PXP, xsize, ysize, APP_IMG_WIDTH, APP_IMG_HEIGHT);
    PXP_SetRotateConfig(APP_PXP, kPXP_RotateProcessSurface, kPXP_Rotate90, kPXP_FlipDisable);
    PXP_SetProcessSurfacePosition(APP_PXP,
                                  APP_PS_ULC_X,
                                  APP_PS_ULC_Y,
                                  APP_PS_ULC_X + APP_IMG_WIDTH - 1U,
                                  APP_PS_ULC_Y + APP_IMG_HEIGHT - 1U);
    outputBufferConfig.buffer0Addr = (uint32_t)s_psBufferLcd[curLcdBufferIdx];
    PXP_SetOutputBufferConfig(APP_PXP, &outputBufferConfig);
    PXP_Start(APP_PXP);

#if VIDEO_PXP_CONV_BLOCKING == 1
#if VIDEO_PXP_CONV_WAITING == 1
    /* Wait for process complete. */
    lcd_time_measure_utility(kLcdTimeType_Start);
    while (!(kPXP_CompleteFlag & PXP_GetStatusFlags(APP_PXP)))
    {
    }
    lcd_time_measure_utility(kLcdTimeType_Pxp);
    PXP_ClearStatusFlags(APP_PXP, kPXP_CompleteFlag);
#else // #if VIDEO_PXP_CONV_WAITING == 0
    if (!isPxpFirstStart)
    {
        PXP_ClearStatusFlags(APP_PXP, kPXP_CompleteFlag);
    }
    else
    {
        isPxpFirstStart = false;
    }
#endif // #if VIDEO_PXP_CONV_WAITING == 1

    LCDIFV2_SetLayerBufferAddr(APP_ELCDIF, APP_ELCDIF_LAYER, (uint32_t)s_psBufferLcd[curLcdBufferIdx]);
    LCDIFV2_TriggerLayerShadowLoad(APP_ELCDIF, APP_ELCDIF_LAYER);
    LCDIFV2_ClearInterruptStatus(APP_ELCDIF, APP_CORE_ID, kLCDIFV2_VerticalBlankingInterrupt);

    curLcdBufferIdx++;
    curLcdBufferIdx %= APP_LCD_FB_NUM;

#if VIDEO_LCD_DISP_BLOCKING == 1
#if VIDEO_LCD_DISP_WAITING == 1
    lcd_time_measure_utility(kLcdTimeType_Start);
    while (!(kLCDIFV2_VerticalBlankingInterrupt & LCDIFV2_GetInterruptStatus(APP_ELCDIF, APP_CORE_ID)))
    {
    }
    lcd_time_measure_utility(kLcdTimeType_Lcd);
#endif // #if VIDEO_LCD_DISP_WAITING == 1
#endif // #if VIDEO_LCD_DISP_BLOCKING == 1
#endif // #if VIDEO_PXP_CONV_BLOCKING == 1
}

void set_pxp_master_priority(uint32_t priority)
{
    // NIC-301(SIM_MAIN GPV) read qos
    *(uint32_t *)0x41046100 = priority;
    // NIC-301(SIM_MAIN GPV) write qos
    *(uint32_t *)0x41046104 = priority;
}

void set_lcd_master_priority(uint32_t priority)
{
    // NIC-301(SIM_MAIN GPV) read qos
    *(uint32_t *)0x41044100 = priority;
    // NIC-301(SIM_MAIN GPV) write qos
    *(uint32_t *)0x41044104 = priority;
}

/*!
 * @brief config_lcd function
 */
void config_lcd(video_lcd_cfg_t *lcdCfg)
{
    BOARD_InitLcdifPixelClock();

    if (lcdCfg->srcWidth > lcdCfg->srcHeight)
    {
        APP_InitPxp(lcdCfg->srcWidth);
    }
    else
    {
        APP_InitPxp(lcdCfg->srcHeight);
    }
    APP_InitLcdif();
    LCDIFV2_EnableInterrupts(APP_ELCDIF, APP_CORE_ID, kLCDIFV2_VerticalBlankingInterrupt);
    BOARD_EnableLcdInterrupt();

#if (VIDEO_LCD_RESOLUTION_WXGA800 == 1) || (VIDEO_LCD_RESOLUTION_SVGA600 == 1)
    set_lcd_master_priority(15);
    set_pxp_master_priority(14);
#elif VIDEO_LCD_RESOLUTION_HVGA272 == 1
    // Do nothing
    set_lcd_master_priority(15);
    set_pxp_master_priority(14);
#endif
}

void APP_LCDIF_IRQHandler(void)
{
    uint32_t intStatus;

    intStatus = LCDIFV2_GetInterruptStatus(APP_ELCDIF, APP_CORE_ID);

    LCDIFV2_ClearInterruptStatus(APP_ELCDIF, APP_CORE_ID, intStatus);

    if (intStatus & kLCDIFV2_VerticalBlankingInterrupt)
    {
        if (g_lcdFramePending)
        {
            /*
             * The inactive buffer turns to be active frame buffer, the
             * old active frame buffer is not used, so put it into the
             * frame buffer list.
             */
            APP_PutFrameBuffer(activeBuf);

            activeBuf = inactiveBuf;
            g_lcdFramePending = false;
        }
    }
}

void LCDIF2_IRQHandler(void)
{
    APP_LCDIF_IRQHandler();
    __DSB();
}
