/*
 * sound\soc\sunxi\daudio0\h3_rt3261.c
 * (C) Copyright 2010-2016
 * Reuuimlla Technology Co., Ltd. <www.reuuimllatech.com>
 * huangxin <huangxin@Reuuimllatech.com>
 *
 * some simple description for this code
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 */

#include <linux/module.h>
#include <linux/clk.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <sound/pcm.h>
#include <sound/soc.h>
#include <sound/pcm_params.h>
#include <sound/soc-dapm.h>
#include <linux/io.h>
#include <mach/sys_config.h>
#include <linux/dma-mapping.h>
#include <linux/dma/sunxi-dma.h>
#include <sound/core.h>
#include <sound/dmaengine_pcm.h>
#include <asm/dma.h>
#include <mach/hardware.h>
#include <video/drv_hdmi.h>
#ifdef CONFIG_HAS_EARLYSUSPEND
#include <linux/earlysuspend.h>
#endif
#include "sunxi-daudio0.h"
#include "sunxi-daudiodma0.h"
#include "../daudio1/sunxi-daudio1.h"
#include "../daudio1/sunxi-daudiodma1.h"
#include "./../hdmiaudio/sunxi-hdmitdm.h"
#include "./../spdif/sunxi_spdif.h"
#include "../../codecs/ac100.h"

static __audio_hdmi_func 	g_hdmi_func;
static hdmi_audio_t 		hdmi_para;
atomic_t pcm_count_num_muti;
struct mutex open_hdmi_mutex;

static atomic_t pcm_active;

static bool daudio_pcm_select 	= 0;
static int daudio_used 			= 0;
static int ac100_used 	= 0;
static int daudio_master 		= 0;
static int audio_format 		= 0;
static int signal_inversion 	= 0;
static bool analog_bb = 0;
static bool digital_bb = 0;
static bool dma_daudio0_to_hdmi_en = false;
static bool dma_daudio0_to_spdif_en = false;

static struct snd_soc_pcm_runtime *runtime = NULL;

struct dmaengine_pcm_runtime_data {
        struct dma_chan *dma_chan;
        dma_cookie_t cookie;

        unsigned int pos;

        void *data;
};
static struct dmaengine_pcm_runtime_data *down_prtdma;

static int daudio2_hdmi_spdif_en(int hdmi_en, int spdif_en);
static int daudio0_set_clk(void);

/*
*	daudio_pcm_select == 0:-->	pcm
*	daudio_pcm_select == 1:-->	i2s
*/
static int sunxi_daudio_set_audio_mode(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	daudio_pcm_select = ucontrol->value.integer.value[0];

	if (daudio_pcm_select) {
		audio_format 			= 1;
		signal_inversion 		= 1;
		daudio_master 			= 4;
	} else {
		audio_format 			= 4;
		signal_inversion 		= 3;
		daudio_master 			= 1;
	}

	return 0;
}

static int sunxi_daudio_get_audio_mode(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = daudio_pcm_select;
	return 0;
}

void audio_set_muti_hdmi_func(__audio_hdmi_func *hdmi_func)
{
	if (hdmi_func) {
		g_hdmi_func.hdmi_audio_enable 	= hdmi_func->hdmi_audio_enable;
		g_hdmi_func.hdmi_set_audio_para = hdmi_func->hdmi_set_audio_para;
		g_hdmi_func.hdmi_is_playback = hdmi_func->hdmi_is_playback;
	} else {
		pr_err("error:%s,line:%d\n", __func__, __LINE__);
	}
}
EXPORT_SYMBOL(audio_set_muti_hdmi_func);

static int i2s_set_dma_daudio0_to_hdmi(int on)
{
	int ret;
	if (on) {
		struct dma_slave_config slave_config;
		down_prtdma = kzalloc(sizeof(*down_prtdma), GFP_KERNEL);
		if (!down_prtdma)
			return -ENOMEM;

		ret = snd_dmaengine_pcm_open_diy(down_prtdma, NULL, NULL);
		if (ret) {
			printk("<0> upstream dmaengine pcm open failed with err %d\n", ret);
			return ret;
		}

		slave_config.dst_addr_width = DMA_SLAVE_BUSWIDTH_2_BYTES;
		slave_config.src_addr_width = DMA_SLAVE_BUSWIDTH_2_BYTES;
		slave_config.dst_addr = 0x01c22820;//hdmi tx addr
		slave_config.src_addr = 0x01c22010;//daudio0 rx addr
		slave_config.direction = DMA_DEV_TO_DEV;
		slave_config.src_maxburst = 4;
		slave_config.dst_maxburst = 4;
		slave_config.slave_id = sunxi_slave_id(DRQDST_DAUDIO_2_TX, DRQSRC_DAUDIO_0_RX);//dst=daudio2-hdmiaudio,src=daudio0

		ret = dmaengine_slave_config(down_prtdma->dma_chan, &slave_config);
		if (ret < 0) {
			printk("<0> dma slave config failed with err %d\n", ret);
			return ret;
		}

		snd_dmaengine_pcm_trigger_diy(down_prtdma, SNDRV_PCM_TRIGGER_START, 0, slave_config.direction, 1024, 512);
	} else {
		if (!down_prtdma) {
			printk("<0> function:%s, down_prtdma = null !!!!\n",__func__);
			return -ENOMEM;
		}
		snd_dmaengine_pcm_trigger_diy(down_prtdma, SNDRV_PCM_TRIGGER_STOP, 0, 0, 1024, 512);
		snd_dmaengine_pcm_close_diy(down_prtdma);
		kfree(down_prtdma);
	}
	return 0;
}

static int daudio0_set_clk(void)
{
	int ret  = 0;
	u32 freq_in = 24000000;
	u32 freq_out = 22579200;
	struct snd_soc_pcm_runtime *rtd = runtime;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	unsigned long sample_rate = 44100;

	switch (sample_rate) {
		case 8000:
		case 16000:
		case 32000:
		case 64000:
		case 128000:
		case 12000:
		case 24000:
		case 48000:
		case 96000:
		case 192000:
			freq_out = 24576000;
			break;
	}

	pr_info("%s,line:%d,freq_in:%d,daudio_pcm_select:%d,audio_format:%d,signal_inversion:%d\n",__func__,__LINE__,
			freq_in,daudio_pcm_select,audio_format,signal_inversion);
	/* set the codec FLL */
	ret = snd_soc_dai_set_pll(codec_dai, AC100_MCLK1, 0, freq_in, freq_out);
	if (ret < 0) {
		return ret;
	}

	/*set cpu_dai clk*/
	ret = snd_soc_dai_set_sysclk(cpu_dai, 0 , freq_out, daudio_pcm_select);
	if (ret < 0) {
		return ret;
	}
	/*set codec_dai clk*/
	ret = snd_soc_dai_set_sysclk(codec_dai, AIF1_CLK , freq_out, daudio_pcm_select);
	if (ret < 0) {
		return ret;
	}
	/*
	* ac100: master. AP: slave
	*/
	ret = snd_soc_dai_set_fmt(codec_dai, SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_CBM_CFM);
	if (ret < 0) {
		return ret;
	}
	ret = snd_soc_dai_set_fmt(cpu_dai, (audio_format | (signal_inversion<<8) | SND_SOC_DAIFMT_CBM_CFM));
	if (ret < 0) {
		return ret;
	}
	ret = snd_soc_dai_set_clkdiv(cpu_dai, 0, sample_rate);
	if (ret < 0) {
		return ret;
	}

	return 0;
}
#define SUNXI_DAUDIO2VBASE 							(0xf1c22800)
extern int tdm2_set_rate(int freq);
extern int tdm2_set_fmt(unsigned int fmt);
extern int tdm2_set_clkdiv(int sample_rate);
extern int tdm2_hw_params(int sample_resolution);
extern void tdm2_tx_enable(int tx_en, int hub_en);
extern int tdm2_prepare(struct snd_pcm_substream *substream);
extern int spdif_set_fmt(unsigned int fmt);
extern int spdif_set_clkdiv(int div_id, int div);
extern int spdif_hw_params(int format);
extern void spdif_txctrl_enable(int tx_en, int chan, int hub_en);

static int tdm2_prepare_tmp(void)
{
	int reg_val;
	reg_val = readl(SUNXI_DAUDIO2VBASE + SUNXI_TXCHCFG);
	reg_val &= ~(0x7<<0);
	reg_val |= (0x1)<<0;
	writel(reg_val, SUNXI_DAUDIO2VBASE + SUNXI_TXCHCFG);

	reg_val = readl(SUNXI_DAUDIO2VBASE + SUNXI_DAUDIOTX0CHSEL);
	reg_val |= (0x1<<12);
	reg_val &= ~(0xff<<4);
	reg_val &= ~(0x7<<0);
	reg_val |= (0x3<<4);
	reg_val	|= (0x1<<0);
	writel(reg_val, SUNXI_DAUDIO2VBASE + SUNXI_DAUDIOTX0CHSEL);

	reg_val = readl(SUNXI_DAUDIO2VBASE + SUNXI_DAUDIOTX0CHMAP);
	reg_val = 0;
	reg_val = 0x10;
	writel(reg_val, SUNXI_DAUDIO2VBASE + SUNXI_DAUDIOTX0CHMAP);

	reg_val = readl(SUNXI_DAUDIO2VBASE + SUNXI_DAUDIOCTL);
	reg_val &= ~SUNXI_DAUDIOCTL_SDO3EN;
	reg_val &= ~SUNXI_DAUDIOCTL_SDO2EN;
	reg_val &= ~SUNXI_DAUDIOCTL_SDO1EN;
	reg_val &= ~SUNXI_DAUDIOCTL_SDO0EN;
	reg_val |= SUNXI_DAUDIOCTL_SDO0EN;
	writel(reg_val, SUNXI_DAUDIO2VBASE + SUNXI_DAUDIOCTL);
	/*clear TX counter*/
	writel(0, SUNXI_DAUDIO2VBASE + SUNXI_DAUDIOTXCNT);
	/* DAUDIO TX ENABLE */
	reg_val = readl(SUNXI_DAUDIO2VBASE + SUNXI_DAUDIOCTL);
	reg_val |= SUNXI_DAUDIOCTL_TXEN;
	writel(reg_val, SUNXI_DAUDIO2VBASE + SUNXI_DAUDIOCTL);

	return 0;
}
atomic_t hdmi_open_num;
static int daudio2_hdmi_spdif_en(int hdmi_en, int spdif_en)
{
	int reg_val = 0;
	int freq = 22579200;
	int sample_rate = 44100;
	int sample_resolution = 16;
	int fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_CBS_CFS;
	unsigned int spdif_mclk_div=0;
	unsigned int spdif_mpll=0, spdif_bclk_div=0, spdif_mult_fs=0;

	if (hdmi_en) {
		tdm2_set_rate(freq);
		tdm2_set_fmt(fmt);
		tdm2_set_clkdiv(sample_rate);
		tdm2_hw_params(sample_resolution);

		hdmi_para.ca 			= 0x0;
		hdmi_para.data_raw 		= 1;
		hdmi_para.channel_num 	= 2;
		hdmi_para.sample_rate 	= sample_rate;
		hdmi_para.sample_bit 	= sample_resolution;
		tdm2_prepare_tmp();
		if (!g_hdmi_func.hdmi_set_audio_para) {
			printk(KERN_WARNING "hdmi video isn't insmod, hdmi interface is null\n");
			return 0;
		}
		/*Global Enable Digital Audio Interface*/
		reg_val = readl(SUNXI_DAUDIO2_VBASE + SUNXI_DAUDIOCTL);
		reg_val |= SUNXI_DAUDIOCTL_GEN;
		writel(reg_val, SUNXI_DAUDIO2_VBASE + SUNXI_DAUDIOCTL);
		msleep(10);
		reg_val = readl(SUNXI_DAUDIO2_VBASE + SUNXI_DAUDIOFCTL);
		reg_val |= SUNXI_DAUDIOFCTL_TXTL(112);
		writel(reg_val, SUNXI_DAUDIO2_VBASE + SUNXI_DAUDIOFCTL);
		/*flush TX FIFO*/
		reg_val = readl(SUNXI_DAUDIO2_VBASE + SUNXI_DAUDIOFCTL);
		reg_val |= SUNXI_DAUDIOFCTL_FTX;
		writel(reg_val, SUNXI_DAUDIO2_VBASE + SUNXI_DAUDIOFCTL);
		/*
		*	set the first pcm param, need set the hdmi audio pcm param
		*	set the data_raw param, need set the hdmi audio raw param
		*/
		mutex_lock(&open_hdmi_mutex);
		if (atomic_read(&hdmi_open_num) < 1) {
			g_hdmi_func.hdmi_set_audio_para(&hdmi_para);
			g_hdmi_func.hdmi_audio_enable(1, 1);
			//tdm2_tx_enable(1, 1);
		}
		//tdm2_tx_enable(1, 1);
		atomic_inc(&hdmi_open_num);
		mutex_unlock(&open_hdmi_mutex);
	} else {
		mutex_lock(&open_hdmi_mutex);
		if(atomic_read(&hdmi_open_num) > 0) {
			atomic_dec(&hdmi_open_num);
			if(atomic_read(&hdmi_open_num) == 0) {
				g_hdmi_func.hdmi_audio_enable(0, 1);
			//	tdm2_tx_enable(0, 0);
			}
		}
		mutex_unlock(&open_hdmi_mutex);
	}

	if (spdif_en) {
		spdif_set_fmt(0);
		get_clock_divder(sample_rate, 32, &spdif_mclk_div, &spdif_mpll, &spdif_bclk_div, &spdif_mult_fs);
		spdif_set_clkdiv(SUNXI_DIV_MCLK, spdif_mclk_div);
		spdif_hw_params(sample_resolution);
		//spdif_txctrl_enable(1, 2, 1);
		//g_hdmi_func.hdmi_audio_enable(0, 1);
	} else {
		//spdif_txctrl_enable(0, 2, 0);
	}

	return 0;
}

/*
 * open/close dma daudio0->hdmi
 */
static int sunxi_i2s_set_daudio0_to_hdmi(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	dma_daudio0_to_hdmi_en = ucontrol->value.integer.value[0];
	if (dma_daudio0_to_hdmi_en) {
		daudio0_set_clk();
		sunxi_snd_rxctrl_daudio0(NULL, 1);
		daudio2_hdmi_spdif_en(1, 0);
	} else {
		sunxi_snd_rxctrl_daudio0(NULL, 0);
		daudio2_hdmi_spdif_en(0, 0);
	}
	return 0;
}

static int sunxi_i2s_get_daudio0_to_hdmi(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = dma_daudio0_to_hdmi_en;
	return 0;
}

/*
 * open/close dma daudio0->spdif
 */
static int sunxi_i2s_set_daudio0_to_spdif(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	dma_daudio0_to_spdif_en = ucontrol->value.integer.value[0];
	if (dma_daudio0_to_spdif_en) {
		i2s_set_dma_daudio0_to_hdmi(1);
		daudio0_set_clk();
		sunxi_snd_rxctrl_daudio0(NULL, 1);
		daudio2_hdmi_spdif_en(1, 1);
	} else {
		sunxi_snd_rxctrl_daudio0(NULL, 0);
		daudio2_hdmi_spdif_en(0, 0);
		i2s_set_dma_daudio0_to_hdmi(0);
	}
	return 0;
}

static int sunxi_i2s_get_daudio0_to_spdif(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = dma_daudio0_to_spdif_en;
	return 0;
}

/* I2s Or Pcm Audio Mode Select */
static const struct snd_kcontrol_new sunxi_daudio_controls[] = {
	SOC_SINGLE_BOOL_EXT("I2s Or Pcm Audio Mode Select format", 0,
			sunxi_daudio_get_audio_mode, sunxi_daudio_set_audio_mode),
	SOC_SINGLE_BOOL_EXT("I2s daudio0 to hdmi", 0, sunxi_i2s_get_daudio0_to_hdmi, sunxi_i2s_set_daudio0_to_hdmi),
	SOC_SINGLE_BOOL_EXT("I2s daudio0 to spdif", 0, sunxi_i2s_get_daudio0_to_spdif, sunxi_i2s_set_daudio0_to_spdif),
};

static int sunxi_snddaudio_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	int ret  = 0;
	u32 freq_in = 24000000;
	u32 freq_out = 22579200;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	int sample_rate = params_rate(params);

	switch (sample_rate) {
		case 8000:
		case 16000:
		case 32000:
		case 64000:
		case 128000:
		case 12000:
		case 24000:
		case 48000:
		case 96000:
		case 192000:
			freq_out = 24576000;
			break;
	}
	pr_debug("%s,line:%d,freq_in:%d,daudio_pcm_select:%d,audio_format:%d,signal_inversion:%d\n",__func__,__LINE__,
			freq_in,daudio_pcm_select,audio_format,signal_inversion);
	/* set the codec FLL */
	ret = snd_soc_dai_set_pll(codec_dai, AC100_MCLK1, 0, freq_in, freq_out);
	if (ret < 0) {
		return ret;
	}

	/*set cpu_dai clk*/
	ret = snd_soc_dai_set_sysclk(cpu_dai, 0 , freq_out, daudio_pcm_select);
	if (ret < 0) {
		return ret;
	}
	/*set codec_dai clk*/
	ret = snd_soc_dai_set_sysclk(codec_dai, AIF1_CLK , freq_out, daudio_pcm_select);
	if (ret < 0) {
		return ret;
	}
	/*
	* ac100: master. AP: slave
	*/
	ret = snd_soc_dai_set_fmt(codec_dai, SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_CBM_CFM);
	if (ret < 0) {
		return ret;
	}
	ret = snd_soc_dai_set_fmt(cpu_dai, (audio_format | (signal_inversion<<8) | SND_SOC_DAIFMT_CBM_CFM));
	if (ret < 0) {
		return ret;
	}
	ret = snd_soc_dai_set_clkdiv(cpu_dai, 0, sample_rate);
	if (ret < 0) {
		return ret;
	}
	return 0;
}

static int sunxi_snddaudio1_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	int ret  = 0;
	u32 freq_in = 24000000;
	u32 freq_out = 22579200;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	unsigned long sample_rate = params_rate(params);

	switch (sample_rate) {
		case 8000:
		case 16000:
		case 32000:
		case 64000:
		case 128000:
		case 12000:
		case 24000:
		case 48000:
		case 96000:
		case 192000:
			freq_out = 24576000;
			break;
	}

	/* set the codec FLL */
	ret = snd_soc_dai_set_pll(codec_dai, AC100_MCLK1, 0, freq_in, freq_out);
	if (ret < 0) {
		return ret;
	}
	/*set system clock source freq and set the mode as daudio or pcm*/
	ret = snd_soc_dai_set_sysclk(cpu_dai, 0 , freq_out, daudio_pcm_select);
	if (ret < 0) {
		return ret;
	}
	ret = codec_dai->driver->ops->set_sysclk(codec_dai, AIF1_CLK, freq_out,
			SND_SOC_CLOCK_IN);
	if (ret < 0)
		return ret;

	ret = snd_soc_dai_set_fmt(codec_dai, SND_SOC_DAIFMT_I2S |
			SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_CBM_CFM);
	if (ret < 0) {
		pr_err("%s, line:%d\n", __func__, __LINE__);
		return ret;
	}
	/*
	* codec clk & FRM master. AP as slave
	*/
	ret = snd_soc_dai_set_fmt(cpu_dai, (audio_format | (signal_inversion<<8) | (daudio_master<<12)));
	if (ret < 0) {
		return ret;
	}
	ret = snd_soc_dai_set_clkdiv(cpu_dai, 0, sample_rate);
	if (ret < 0) {
		return ret;
	}

	/*
	*	audio_format == SND_SOC_DAIFMT_DSP_A
	*	signal_inversion<<8 == SND_SOC_DAIFMT_IB_NF
	*	daudio_master<<12 == SND_SOC_DAIFMT_CBM_CFM
	*/
	DAUDIO_DBG("%s,line:%d,audio_format:%d,SND_SOC_DAIFMT_DSP_A:%d\n",\
			__func__, __LINE__, audio_format, SND_SOC_DAIFMT_DSP_A);
	DAUDIO_DBG("%s,line:%d,signal_inversion:%d,signal_inversion<<8:%d,SND_SOC_DAIFMT_IB_NF:%d\n",\
			__func__, __LINE__, signal_inversion, signal_inversion<<8, SND_SOC_DAIFMT_IB_NF);
	DAUDIO_DBG("%s,line:%d,daudio_master:%d,daudio_master<<12:%d,SND_SOC_DAIFMT_CBM_CFM:%d\n",\
			__func__, __LINE__, daudio_master, daudio_master<<12, SND_SOC_DAIFMT_CBM_CFM);

	return 0;
}

static int phone_system_voice_hw_params(struct snd_pcm_substream *substream,
		struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	int ret = 0;

	/* set codec DAI configuration */
	ret = snd_soc_dai_set_fmt(codec_dai, SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_CBM_CFM);
	if (ret < 0)
		return ret;
	return 0;
}

static const struct snd_kcontrol_new ac100_pin_controls[] = {
	SOC_DAPM_PIN_SWITCH("External Speaker"),
	SOC_DAPM_PIN_SWITCH("Headphone"),
	SOC_DAPM_PIN_SWITCH("Earpiece"),
};

static const struct snd_soc_dapm_widget a80_ac100_dapm_widgets[] = {
	SND_SOC_DAPM_MIC("External MainMic", NULL),
	SND_SOC_DAPM_MIC("HeadphoneMic", NULL),
	SND_SOC_DAPM_MIC("DigitalMic", NULL),
};

static const struct snd_soc_dapm_route audio_map[] = {
	{"MainMic Bias", NULL, "External MainMic"},
	{"MIC1P", NULL, "MainMic Bias"},
	{"MIC1N", NULL, "MainMic Bias"},

	{"MIC2", NULL, "HMic Bias"},
	{"HMic Bias", NULL, "HeadphoneMic"},

	/*d-mic*/
	{"MainMic Bias", NULL, "DigitalMic"},
	{"D_MIC", NULL, "MainMic Bias"},
};

/*
 * Card initialization
 */
static int sunxi_daudio_init(struct snd_soc_pcm_runtime *rtd)
{
	int ret;
	struct snd_soc_codec *codec = rtd->codec;
	struct snd_soc_dapm_context *dapm = &codec->dapm;
	struct snd_soc_card *card = rtd->card;

	runtime = rtd;
	snd_soc_dapm_disable_pin(&codec->dapm,	"HPOUTR");
	snd_soc_dapm_disable_pin(&codec->dapm,	"HPOUTL");
	snd_soc_dapm_disable_pin(&codec->dapm,	"EAROUTP");
	snd_soc_dapm_disable_pin(&codec->dapm,	"EAROUTN");
	snd_soc_dapm_disable_pin(&codec->dapm,	"SPK1P");
	snd_soc_dapm_disable_pin(&codec->dapm,	"SPK2P");
	snd_soc_dapm_disable_pin(&codec->dapm,	"SPK1N");
	snd_soc_dapm_disable_pin(&codec->dapm,	"SPK2N");
	snd_soc_dapm_disable_pin(&codec->dapm,	"MIC1P");
	snd_soc_dapm_disable_pin(&codec->dapm,	"MIC1N");
	snd_soc_dapm_disable_pin(&codec->dapm,	"MIC2");
	snd_soc_dapm_disable_pin(&codec->dapm,	"MIC3");
	snd_soc_dapm_disable_pin(&codec->dapm,	"D_MIC");
	/* Add HDMI/SPDIF switch */
	ret = snd_soc_add_codec_controls(codec, sunxi_daudio_controls,
					ARRAY_SIZE(sunxi_daudio_controls));
	if (ret) {
		dev_warn(card->dev,
				"Failed to register audio mode control, "
				"will continue without it.\n");
	}
	snd_soc_dapm_sync(dapm);
	atomic_set(&hdmi_open_num, 0);
	return 0;
}

#ifdef CONFIG_HAS_EARLYSUSPEND
void rt3261_early_suspend(struct early_suspend *h) {
#ifdef CONFIG_ARCH_SUN8IW7
	if (dma_daudio0_to_spdif_en||dma_daudio0_to_hdmi_en) {
		g_hdmi_func.hdmi_audio_enable(0, 1);
	}
	atomic_set(&hdmi_open_num, 0);
#endif
}

void rt3261_late_resume(struct early_suspend *h) {
#ifdef CONFIG_ARCH_SUN8IW7
		if (dma_daudio0_to_spdif_en||dma_daudio0_to_hdmi_en) {
			g_hdmi_func.hdmi_set_audio_para(&hdmi_para);
			g_hdmi_func.hdmi_audio_enable(1, 1);
		}
#endif
}

 static struct early_suspend rt3261_early_suspend_handler =
{
	.level   = EARLY_SUSPEND_LEVEL_BLANK_SCREEN,
	.suspend = rt3261_early_suspend,
	.resume = rt3261_late_resume,
};
#endif
static int sunxi_snddaudio_hw_free(struct snd_pcm_substream *substream )
{
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		atomic_dec(&pcm_active);
		if (atomic_read(&pcm_active) == 0) {
			tdm2_tx_enable(0, 0);
			spdif_txctrl_enable(0, 2, 0);
		}
	}
	return 0;
}
static int sunxi_snddaudio_prepare(struct snd_pcm_substream *substream )
{
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		msleep(100);
		atomic_inc(&pcm_active);
		tdm2_tx_enable(1, 1);
		spdif_txctrl_enable(1, 2, 1);
	}else
	msleep(500);
	return 0;
}

static struct snd_soc_ops sunxi_snddaudio_ops = {
	.prepare		= sunxi_snddaudio_prepare,
	.hw_params 		= sunxi_snddaudio_hw_params,
	.hw_free		= sunxi_snddaudio_hw_free,
};
static struct snd_soc_ops sunxi_snddaudio1_ops = {
	.hw_params 		= sunxi_snddaudio1_hw_params,
	.prepare		= sunxi_snddaudio_prepare,
	.hw_free		= sunxi_snddaudio_hw_free,
};

static struct snd_soc_ops phone_system_voice_ops = {
	.hw_params = phone_system_voice_hw_params,

};

static struct snd_soc_dai_link sunxi_snddaudio_dai_link[] = {
	{
	.name 			= "s_i2s1",
		.cpu_dai_name 	= "pri_dai",
		.stream_name 	= "SUNXI-I2S0",
		.codec_dai_name = "ac100-aif1",
		.codec_name 	= "ac100-codec",
	.init 			= sunxi_daudio_init,
	.platform_name 	= "sunxi-daudio-pcm-audio.0",
	.ops 			= &sunxi_snddaudio_ops,
	},
	{ /* Second DAI i/f */
		.name = "ac100 Voice",
		.stream_name = "Voice",
		.cpu_dai_name 	= "pcm1",
		.codec_dai_name = "ac100-aif2",
		.codec_name = "ac100-codec",
		.platform_name 	= "sunxi-daudio-pcm-audio1.0",
		.ops 			= &sunxi_snddaudio1_ops,
	} ,
	 {/*daudio0 to daudio2 IO-IO hdmi out*/
		.name = "VIR",
		.cpu_dai_name 	= "bb-voice-dai",
		.stream_name 	= "vir-dai",
		.codec_dai_name = "ac100-aif1",
		.codec_name 	= "ac100-codec",
		.ops 			= &phone_system_voice_ops,
	} ,
};

static struct snd_soc_card snd_soc_sunxi_snddaudio = {
	.name 		= "sndac100",
	.owner 		= THIS_MODULE,
	.dai_link 	= sunxi_snddaudio_dai_link,
	.num_links 	= ARRAY_SIZE(sunxi_snddaudio_dai_link),
	.dapm_widgets = a80_ac100_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(a80_ac100_dapm_widgets),
	.dapm_routes = audio_map,
	.num_dapm_routes = ARRAY_SIZE(audio_map),
	.controls = ac100_pin_controls,
	.num_controls = ARRAY_SIZE(ac100_pin_controls),
};

static int __devinit sunxi_snddaudio0_dev_probe(struct platform_device *pdev)
{
	int ret = 0;
	script_item_u val;
	script_item_value_type_e  type;
	struct snd_soc_card *card = &snd_soc_sunxi_snddaudio;

	type = script_get_item(TDM_NAME, "daudio_used", &val);
	if (SCIRPT_ITEM_VALUE_TYPE_INT != type) {
		pr_err("[daudio0]:%s,line:%d type err!\n", __func__, __LINE__);
	}
	daudio_used = val.val;
	type = script_get_item(TDM_NAME, "daudio_select", &val);
	if (SCIRPT_ITEM_VALUE_TYPE_INT != type) {
		pr_err("[I2S0] daudio_select type err!\n");
	}
	daudio_pcm_select = val.val;

	type = script_get_item(TDM_NAME, "daudio_master", &val);
	if (SCIRPT_ITEM_VALUE_TYPE_INT != type) {
		pr_err("[I2S0] daudio_master type err!\n");
	}
	daudio_master = val.val;

	type = script_get_item(TDM_NAME, "audio_format", &val);
	if (SCIRPT_ITEM_VALUE_TYPE_INT != type) {
		pr_err("[I2S0] audio_format type err!\n");
	}
	audio_format = val.val;

	type = script_get_item(TDM_NAME, "signal_inversion", &val);
	if (SCIRPT_ITEM_VALUE_TYPE_INT != type) {
		pr_err("[I2S0] signal_inversion type err!\n");
	}
	signal_inversion = val.val;

	type = script_get_item("ac100_audio0", "analog_bb", &val);
	if (SCIRPT_ITEM_VALUE_TYPE_INT != type) {
		pr_err("[ac100_audio0] analog_bb type err!\n");
	}
	analog_bb = val.val;

	type = script_get_item("ac100_audio0", "digital_bb", &val);
	if (SCIRPT_ITEM_VALUE_TYPE_INT != type) {
		pr_err("[ac100_audio0] digital_bb type err!\n");
	}
	digital_bb = val.val;
 #ifdef CONFIG_HAS_EARLYSUSPEND
	register_early_suspend(&rt3261_early_suspend_handler);
 #endif
	mutex_init(&open_hdmi_mutex);
	atomic_set(&pcm_active, 0);

	if (daudio_used) {
		card->dev = &pdev->dev;
	
		ret = snd_soc_register_card(card);
		if (ret) {
			dev_err(&pdev->dev, "snd_soc_register_card() failed: %d\n",
				ret);
		}
	} else {
		pr_err("[daudio0]a83_ac100 cannot find any using configuration for controllers, return directly!\n");
        return 0;
	}
	return ret;
}

static int __devexit sunxi_snddaudio0_dev_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);

	if (daudio_used) {
		snd_soc_unregister_card(card);
	}
	return 0;
}

/*data relating*/
static struct platform_device sunxi_daudio_device = {
	.name 	= "snddaudio",
	.id 	= PLATFORM_DEVID_NONE,
};

/*method relating*/
static struct platform_driver sunxi_daudio_driver = {
	.probe = sunxi_snddaudio0_dev_probe,
	.remove = __exit_p(sunxi_snddaudio0_dev_remove),
	.driver = {
		.name = "snddaudio",
		.owner = THIS_MODULE,
		.pm = &snd_soc_pm_ops,
	},
};

static int __init sunxi_snddaudio0_init(void)
{
	int err = 0;
	script_item_u val;
	script_item_value_type_e  type;

	type = script_get_item("acx0", "ac100_used", &val);
	if (SCIRPT_ITEM_VALUE_TYPE_INT != type) {
		pr_err("[acx0] ac100_used type err!\n");
	}
	ac100_used = val.val;
	if (ac100_used) {
		if((err = platform_device_register(&sunxi_daudio_device)) < 0)
			return err;

		if ((err = platform_driver_register(&sunxi_daudio_driver)) < 0)
			return err;
	}
	return 0;
}
module_init(sunxi_snddaudio0_init);

static void __exit sunxi_snddaudio0_exit(void)
{
	platform_driver_unregister(&sunxi_daudio_driver);
}
module_exit(sunxi_snddaudio0_exit);
MODULE_AUTHOR("huangxin");
MODULE_DESCRIPTION("SUNXI_snddaudio ALSA SoC audio driver");
MODULE_LICENSE("GPL");
