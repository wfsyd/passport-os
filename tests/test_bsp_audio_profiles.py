"""Compile the entire production BSP driver; inject failure at each init step."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
def strip_host_directives(text):
    return re.sub(r'^(#include .*|#pragma once)$', '', text, flags=re.M)

source = strip_host_directives((ROOT / 'components/bsp/src/bsp_audio.c').read_text())
sleep_check = (strip_host_directives((ROOT / 'components/bsp/src/bsp_es8311_sleep_check.h').read_text()) +
               strip_host_directives((ROOT / 'components/bsp/src/bsp_es8311_sleep_check.c').read_text()))
prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM -2
#define ESP_ERR_INVALID_STATE -3
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define portMAX_DELAY 0
#define pdMS_TO_TICKS(x) (x)
#define portMUX_INITIALIZER_UNLOCKED 0
#define taskENTER_CRITICAL(x) ((void)(x))
#define taskEXIT_CRITICAL(x) ((void)(x))
#define BSP_I2S_PORT 0
#define BSP_I2C_PORT 0
#define BSP_I2C_ES8311_ADDR 24
#define BSP_I2S_PA_CTRL 3
#define BSP_I2S_MCLK 1
#define BSP_I2S_BCLK 2
#define BSP_I2S_WS 3
#define BSP_I2S_DOUT 4
#define BSP_I2S_DIN 5
#define I2S_GPIO_UNUSED -1
#define I2S_ROLE_MASTER 1
#define I2S_CLK_SRC_DEFAULT 1
#define I2S_MCLK_MULTIPLE_256 256
#define I2S_DATA_BIT_WIDTH_16BIT 16
#define I2S_SLOT_BIT_WIDTH_AUTO 0
#define I2S_SLOT_MODE_STEREO 2
#define I2S_STD_SLOT_BOTH 3
#define ESP_CODEC_DEV_WORK_MODE_BOTH 3
#define ESP_CODEC_DEV_WORK_MODE_ADC 1
#define ESP_CODEC_DEV_WORK_MODE_DAC 2
#define ESP_CODEC_DEV_TYPE_IN_OUT 3
#define ESP_CODEC_DEV_TYPE_IN 1
#define ESP_CODEC_DEV_TYPE_OUT 2
#define ESP_CODEC_DEV_OK 0
#define GPIO_MODE_INPUT 1
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0
typedef int esp_err_t,portMUX_TYPE,StaticSemaphore_t,SemaphoreHandle_t;
typedef int i2s_chan_handle_t;
typedef void *esp_codec_dev_handle_t;
static int xSemaphoreCreateRecursiveMutexStatic(void *s){(void)s;return 1;}
static void xSemaphoreTakeRecursive(int a,int b){(void)a;(void)b;}
static void xSemaphoreGiveRecursive(int a){(void)a;}
static void vTaskDelay(int n){(void)n;}
static int steps,fail_at,live,channels,mode,device_type,curve,gain,volume,reads,writes;
static int fail_step(void){return ++steps==fail_at;}
static int bsp_i2c_init(void){return fail_step()?ESP_FAIL:ESP_OK;}
static void *bsp_i2c_bus(void){return NULL;}
typedef struct audio_codec_ctrl_if_t {
 int (*write_reg)(const struct audio_codec_ctrl_if_t*,int,int,const void*,int);
 int (*read_reg)(const struct audio_codec_ctrl_if_t*,int,int,void*,int);
} audio_codec_ctrl_if_t;
/* Fake ES8311 register file: writes store, reads return the stored value. */
static uint8_t regs[256];
static int corrupt_reg=-1,gpio_calls,gpio_mask_has_mclk;
static int reg_write(const audio_codec_ctrl_if_t *a,int reg,int len,const void*v,int n){(void)a;(void)len;(void)n;regs[reg]=(reg==corrupt_reg)?0:*(const uint8_t*)v;return 0;}
static int reg_read(const audio_codec_ctrl_if_t *a,int reg,int len,void*v,int n){(void)a;(void)len;(void)n;*(uint8_t*)v=regs[reg];return 0;}
static audio_codec_ctrl_if_t ctrl={reg_write,reg_read};
typedef struct {uint64_t pin_bit_mask;int mode,pull_up_en,pull_down_en,intr_type;} gpio_config_t;
static int gpio_config(const gpio_config_t*c){gpio_calls++;if(c->pin_bit_mask&(1ULL<<BSP_I2S_MCLK))gpio_mask_has_mclk=1;return 0;}
typedef int audio_codec_data_if_t,audio_codec_if_t,audio_codec_gpio_if_t;
typedef struct {int port,addr;void *bus_handle;} audio_codec_i2c_cfg_t;
typedef struct {int port;i2s_chan_handle_t tx_handle,rx_handle;} audio_codec_i2s_cfg_t;
typedef struct {const void *ctrl_if,*gpio_if;int codec_mode,pa_pin;bool pa_reverted,master_mode,use_mclk;struct {float pa_voltage,codec_dac_voltage;}hw_gain;bool no_dac_ref;} es8311_codec_cfg_t;
typedef struct {int dev_type;const void *codec_if,*data_if;}esp_codec_dev_cfg_t;
typedef struct {uint8_t bits_per_sample,channel;int channel_mask;uint32_t sample_rate;int mclk_multiple;}esp_codec_dev_sample_info_t;
typedef struct {int id,role,dma_desc_num,dma_frame_num;bool auto_clear_after_cb,auto_clear_before_cb;int intr_priority;}i2s_chan_config_t;
typedef struct {
 struct {int sample_rate_hz,clk_src,ext_clk_freq_hz,mclk_multiple;}clk_cfg;
 struct {int data_bit_width,slot_bit_width,slot_mode,slot_mask,ws_width;bool ws_pol,bit_shift,left_align,big_endian,bit_order_lsb;}slot_cfg;
 struct {int mclk,bclk,ws,dout,din;struct {bool mclk_inv,bclk_inv,ws_inv;}invert_flags;}gpio_cfg;
}i2s_std_config_t;
static int data,gpio,codec,dev;
static const audio_codec_ctrl_if_t *audio_codec_new_i2c_ctrl(const audio_codec_i2c_cfg_t*c){(void)c;if(fail_step())return NULL;live++;return &ctrl;}
static int i2s_new_channel(const i2s_chan_config_t*c,i2s_chan_handle_t *tx,i2s_chan_handle_t *rx){
 assert(c->dma_desc_num==4 || c->dma_desc_num==6);if(fail_step())return ESP_FAIL;
 if(tx){*tx=1;channels|=1;}if(rx){*rx=2;channels|=2;}return 0;
}
static int i2s_channel_init_std_mode(int h,const i2s_std_config_t*c){assert(channels&h);assert((c->gpio_cfg.dout!=I2S_GPIO_UNUSED)==!!(channels&1));assert((c->gpio_cfg.din!=I2S_GPIO_UNUSED)==!!(channels&2));return fail_step()?ESP_FAIL:0;}
static int i2s_channel_enable(int h){assert(channels&h);return fail_step()?ESP_FAIL:0;}
static int i2s_channel_disable(int h){assert(channels&h);return 0;}
static int i2s_del_channel(int h){assert(channels&h);channels&=~h;return 0;}
static const audio_codec_data_if_t *audio_codec_new_i2s_data(const audio_codec_i2s_cfg_t*c){assert(c->tx_handle || c->rx_handle);if(fail_step())return NULL;live++;return &data;}
static const audio_codec_gpio_if_t *audio_codec_new_gpio(void){if(fail_step())return NULL;live++;return &gpio;}
static const audio_codec_if_t *es8311_codec_new(const es8311_codec_cfg_t*c){mode=c->codec_mode;assert(c->no_dac_ref);if(fail_step())return NULL;live++;return &codec;}
static void *esp_codec_dev_new(const esp_codec_dev_cfg_t*c){device_type=c->dev_type;if(fail_step())return NULL;live++;return &dev;}
static void audio_codec_delete_ctrl_if(const void*p){assert(p&&live>0);live--;}
static void audio_codec_delete_data_if(const void*p){assert(p&&live>0);live--;}
static void audio_codec_delete_gpio_if(const void*p){assert(p&&live>0);live--;}
static void audio_codec_delete_codec_if(const void*p){assert(p&&live>0);live--;}
static void esp_codec_dev_delete(void*p){assert(p&&live>0);live--;}
static int esp_codec_dev_close(void*p){assert(p);return 0;}
static int esp_codec_dev_open(void*p,const esp_codec_dev_sample_info_t*f){assert(p&&f->sample_rate);return fail_step()?ESP_FAIL:0;}
static int esp_codec_dev_set_in_gain(void*p,float g){assert(p&&g==30);gain++;return 0;}
static int bsp_audio_volume_configure(void*p){assert(p);curve++;return fail_step()?ESP_FAIL:0;}
static uint8_t bsp_audio_volume_effective(uint8_t a,uint8_t b){return a*b/100;}
static int bsp_audio_volume_apply(void*p,uint8_t a,uint8_t b){assert(p);(void)a;(void)b;volume++;return fail_step()?ESP_FAIL:0;}
static int esp_codec_dev_write(void*p,void*d,size_t n){assert(p);(void)d;(void)n;writes++;return 0;}
static int esp_codec_dev_read(void*p,void*d,size_t n){assert(p);(void)d;(void)n;reads++;return 0;}
void bsp_audio_deinit(void);
'''
sleep_prelude = sleep_check
checks = r'''
int main(void){
 int (*init[])(void)={bsp_audio_init,bsp_audio_init_capture,bsp_audio_init_playback,bsp_audio_init_feedback};
 const int directions[]={3,2,1,1};const int modes[]={3,1,2,2};
 for(int p=0;p<4;p++){
  steps=fail_at=0;curve=gain=volume=0;
  assert(init[p]()==ESP_OK);int init_steps=steps;
  assert(channels==directions[p] && mode==modes[p] && device_type==modes[p]);
  int before=steps;assert(init[(p+1)%4]()==ESP_ERR_INVALID_STATE && steps==before);
  assert(bsp_audio_set_format(16000,16,1)==ESP_OK);before=steps;
  assert(bsp_audio_set_format(16000,16,1)==ESP_OK && steps==before);
  assert(gain==!!(channels&2) && curve==!!(channels&1));
  assert((bsp_audio_read(NULL,0)==ESP_OK)==!!(channels&2));
  assert((bsp_audio_write(NULL,0)==ESP_OK)==!!(channels&1));
  assert(bsp_audio_set_format(24000,16,1)==ESP_OK);
  const int live_before_sleep=live;
  corrupt_reg=-1;assert(bsp_audio_prepare_sleep()==ESP_OK);
  assert(!s_opened&&live==live_before_sleep&&regs[0x45]==0x01&&regs[0x0E]==0xFF);
  /* An unverifiable suspend retries once and reports failure; the lock stays held. */
  corrupt_reg=0x45;assert(bsp_audio_prepare_sleep()==ESP_FAIL);corrupt_reg=-1;
  gpio_calls=0;assert(bsp_audio_prepare_deep_sleep()==ESP_OK&&gpio_calls==1&&gpio_mask_has_mclk);
  control_unlock(); /* host fixture resumes only to exercise cleanup */
  bsp_audio_deinit();bsp_audio_deinit();assert(!live && !channels);
  for(int failure=1;failure<=init_steps;failure++){
   steps=0;fail_at=failure;assert(init[p]()!=ESP_OK);
   assert(!live && !channels && !s_dev && !s_ctrl && !s_data && !s_codec && !s_gpio);
   bsp_audio_deinit();steps=fail_at=0;assert(init[p]()==ESP_OK);bsp_audio_deinit();
  }
  steps=fail_at=0;assert(init[p]()==ESP_OK);fail_at=steps+1;
  assert(bsp_audio_set_format(16000,16,1)!=ESP_OK);bsp_audio_deinit();assert(!live&&!channels);
 }
 assert(bsp_audio_read(NULL,0)==ESP_ERR_INVALID_STATE && bsp_audio_write(NULL,0)==ESP_ERR_INVALID_STATE);
 /* Alerts ignore master level (even 0) and end with feedback. */
 steps=fail_at=0;bsp_audio_set_volume(60);bsp_audio_set_alert_volume(90);assert(output_volume()==60*80/100);
 bsp_audio_set_master_volume(20);assert(bsp_audio_init_feedback()==ESP_OK);
 bsp_audio_set_alert_volume(90);assert(output_volume()==90);
 bsp_audio_set_master_volume(0);assert(output_volume()==90);bsp_audio_set_master_volume(20);
 bsp_audio_deinit();assert(!s_alert_volume && output_volume()==60*20/100);bsp_audio_set_master_volume(80);
 puts("BSP capture/playback/duplex/feedback directions, profile conflict, init failure rollback and retry, alert volume: PASS");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp) / 'audio.c'
    path.write_text(prelude + sleep_prelude + source + checks)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-variable',
                    '-Wno-int-conversion', '-fsanitize=address,undefined', str(path), '-o', str(path.with_suffix(''))], check=True)
    subprocess.run([str(path.with_suffix(''))], check=True)
