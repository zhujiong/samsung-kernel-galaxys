
#include <linux/kernel.h>
#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/time.h>

#include <mach/gpio.h>
#include "Si4709_regs.h"
#include "Si4709_main.h"
#include "Si4709_dev.h"
#include "Si4709_common.h"

enum
{
    eTRUE,
    eFALSE,
}dev_struct_status_t;

/*dev_state*/
/*power_state*/
#define RADIO_ON            1
#define RADIO_POWERDOWN     0
/*seek_state*/
#define RADIO_SEEK_ON       1
#define RADIO_SEEK_OFF      0

#define FREQ_87500_kHz      8750
#define FREQ_76000_kHz      7600

#define RSSI_seek_th_MAX    0x7F
#define RSSI_seek_th_MIN    0x00

#define seek_SNR_th_DISB    0x00
#define seek_SNR_th_MIN     0x01  /*most stops*/
#define seek_SNR_th_MAX     0x0F  /*fewest stops*/

#define seek_FM_ID_th_DISB  0x00
#define seek_FM_ID_th_MAX   0x01  /*most stops*/
#define seek_FM_ID_th_MIN   0x0F  /*fewest stops*/

#define TUNE_RSSI_THRESHOLD		0x00
#define TUNE_SNR_THRESHOLD		0x01
#define TUNE_CNT_THRESHOLD		0x00

typedef struct 
{
    u32 frequency;
    u8 rsssi_val;
}channel_into_t;

typedef struct
{
    u16 band;
    u32 bottom_of_band;
    u16 channel_spacing;
    u32 timeout_RDS;      /****For storing the jiffy value****/
    u32 seek_preset[NUM_SEEK_PRESETS];
    u8 curr_snr;
    u8 curr_rssi_th;
}dev_settings_t;

typedef struct 
{
    /*Any function which 
      - views/modifies the fields of this structure
      - does i2c communication 
        should lock the mutex before doing so.
        Recursive locking should not be done.
        In this file all the exported functions will take care
        of this. The static functions will not deal with the
        mutex*/
    struct mutex lock;
	
    struct i2c_client const *client;

    dev_state_t state;

    dev_settings_t settings;

    channel_into_t rssi_freq[50];

    u16 registers[NUM_OF_REGISTERS];

    /* This field will be checked by all the functions
       exported by this file (except the init function), 
       to validate the the fields of this structure. 
       if eTRUE: the fileds are valid
       if eFALSE: do not trust the values of the fields 
       of this structure*/
    unsigned short valid;

    /*will be true is the client ans state fields are correct*/
    unsigned short valid_client_state;
}Si4709_device_t;


/*extern functions*/
/**********************************************/
/*All the exported functions which view or modify the device
  state/data, do i2c com will have to lock the mutex before 
  doing so*/
/**********************************************/
int Si4709_dev_init(struct i2c_client *);
int Si4709_dev_exit(void);

void Si4709_dev_mutex_init(void); 

int Si4709_dev_suspend(void);
int Si4709_dev_resume(void);

int Si4709_dev_powerup(void);
int Si4709_dev_powerdown(void);

int Si4709_dev_band_set(int);
int Si4709_dev_ch_spacing_set(int);

int Si4709_dev_chan_select(u32);
int Si4709_dev_chan_get(u32*);

int Si4709_dev_seek_up(u32*);
int Si4709_dev_seek_down(u32*);
int Si4709_dev_seek_auto(u32*);

int Si4709_dev_RSSI_seek_th_set(u8);
int Si4709_dev_seek_SNR_th_set(u8);
int Si4709_dev_seek_FM_ID_th_set(u8);
int Si4709_dev_cur_RSSI_get(rssi_snr_t*);
int Si4709_dev_VOLEXT_ENB(void);
int Si4709_dev_VOLEXT_DISB(void);
int Si4709_dev_volume_set(u8);
int Si4709_dev_volume_get(u8 *);
int Si4709_dev_MUTE_ON(void);
int Si4709_dev_MUTE_OFF(void);
int Si4709_dev_MONO_SET(void);
int Si4709_dev_STEREO_SET(void);
int Si4709_dev_RDS_ENABLE(void);
int Si4709_dev_RDS_DISABLE(void);
int Si4709_dev_RDS_timeout_set(u32);
int Si4709_dev_rstate_get(dev_state_t*);
int Si4709_dev_RDS_data_get(radio_data_t*);
int Si4709_dev_device_id(device_id *);          	
int Si4709_dev_chip_id(chip_id *);					
int Si4709_dev_sys_config2(sys_config2 *);  		
int Si4709_dev_power_config(power_config *);		
int Si4709_dev_AFCRL_get(u8*);
int Si4709_dev_DE_set(u8);
/***********************************************/

/*static functions*/
/**********************************************/
static void wait(void);

static void wait_RDS(void );

static int powerup(void);
static int powerdown(void);

static int seek(u32*, int);
static int tune_freq(u32);

static void get_cur_chan_freq(u32 *, u16);

static u16 freq_to_channel(u32);
static u32 channel_to_freq(u16);

static int insert_preset(u32,u8,u8*);

static int i2c_read(u8);
static int i2c_write(u8);
/**********************************************/

/*Si4709 device structure*/
static Si4709_device_t Si4709_dev =
{
    .client = NULL,
    .valid = eFALSE,
    .valid_client_state = eFALSE,
};

/*Wait flag*/
/*WAITING or WAIT_OVER or NO_WAIT*/
int Si4709_dev_wait_flag = NO_WAIT;

int Si4709_dev_init(struct i2c_client *client)
{
    int ret = 0;

    debug("Si4709_dev_init called");

    mutex_lock(&(Si4709_dev.lock));
     
    Si4709_dev.client = client;

    /***reset the device here****/
    gpio_set_value(FM_RESET, GPIO_LEVEL_LOW);	
    mdelay(1);
    gpio_set_value(FM_RESET, GPIO_LEVEL_HIGH);	
    mdelay(2);
										
    Si4709_dev.state.power_state = RADIO_POWERDOWN;
    Si4709_dev.state.seek_state = RADIO_SEEK_OFF;

    if( (ret = i2c_read(BOOTCONFIG) ) < 0 )
    {
        debug("i2c_read failed");
    }
    else
    {
        Si4709_dev.valid_client_state = eTRUE;
    }
    
    mutex_unlock(&(Si4709_dev.lock));   	 

    debug("Si4709_dev_init call over");
	 
    return ret;
}

int Si4709_dev_exit(void)
{
	int ret = 0;
	 
	debug("Si4709_dev_exit called");

	mutex_lock(&(Si4709_dev.lock));   

	// Temporary blocked by abnormal function call(E-CIM 2657654) - DW Shim. 2010.03.04
	printk(KERN_ERR "Si4709_dev_exit() is called!!");
//    Si4709_dev.client = NULL;	

//    Si4709_dev.valid_client_state = eFALSE;
//    Si4709_dev.valid = eFALSE;

	mutex_unlock(&(Si4709_dev.lock)); 

	debug("Si4709_dev_exit call over");

	return ret;
}

void Si4709_dev_mutex_init(void)
{
    mutex_init(&(Si4709_dev.lock));
}

int Si4709_dev_powerup(void)
{
    int ret = 0;
    u32 value = 100;

    debug("Si4709_dev_powerup called");

    mutex_lock(&(Si4709_dev.lock)); 

    if(!(RADIO_ON==Si4709_dev.state.power_state))
    {
	if( (ret = powerup()) < 0 )
	{
		debug("powerup failed");
	}
	else if( Si4709_dev.valid_client_state == eFALSE )
	{
		debug("Si4709_dev_powerup called when DS (state, client) is invalid");
		ret = -1;	  
	}    
	else
	{
		/*initial settings*/
#if 0
        POWERCFG_BITSET_RDSM_LOW(&Si4709_dev.registers[POWERCFG]);
#else
        POWERCFG_BITSET_RDSM_HIGH(&Si4709_dev.registers[POWERCFG]);
#endif
	//   POWERCFG_BITSET_SKMODE_HIGH(&Si4709_dev.registers[POWERCFG]);
	POWERCFG_BITSET_SKMODE_LOW(&Si4709_dev.registers[POWERCFG]); /*VNVS:18-NOV'09---- wrap at the upper and lower band limit and continue seeking*/
	SYSCONFIG1_BITSET_STCIEN_HIGH(&Si4709_dev.registers[SYSCONFIG1]);
	SYSCONFIG1_BITSET_RDSIEN_LOW(&Si4709_dev.registers[SYSCONFIG1]);
	SYSCONFIG1_BITSET_RDS_HIGH(&Si4709_dev.registers[SYSCONFIG1]);
	SYSCONFIG1_BITSET_DE_50(&Si4709_dev.registers[SYSCONFIG1]); /*VNVS:18-NOV'09--- Setting DE-Time Constant as 50us(Europe,Japan,Australia)*/
	SYSCONFIG1_BITSET_GPIO_STC_RDS_INT(&Si4709_dev.registers[SYSCONFIG1]);
	SYSCONFIG1_BITSET_RESERVED(&Si4709_dev.registers[SYSCONFIG1]);
        
	//    SYSCONFIG2_BITSET_SEEKTH(&Si4709_dev.registers[SYSCONFIG2],2);
	SYSCONFIG2_BITSET_SEEKTH(&Si4709_dev.registers[SYSCONFIG2], TUNE_RSSI_THRESHOLD); /*VNVS:18-NOV'09---- modified for detecting more stations of good quality*/
	SYSCONFIG2_BITSET_VOLUME(&Si4709_dev.registers[SYSCONFIG2],0x0F);
	SYSCONFIG2_BITSET_BAND_87p5_108_MHz(&Si4709_dev.registers[SYSCONFIG2]);
	Si4709_dev.settings.band = BAND_87500_108000_kHz;
	Si4709_dev.settings.bottom_of_band = FREQ_87500_kHz;
        
	SYSCONFIG2_BITSET_SPACE_100_KHz(&Si4709_dev.registers[SYSCONFIG2]);
	Si4709_dev.settings.channel_spacing = CHAN_SPACING_100_kHz;         				


	//  SYSCONFIG3_BITSET_SKSNR(&Si4709_dev.registers[SYSCONFIG3],3);
	SYSCONFIG3_BITSET_SKSNR(&Si4709_dev.registers[SYSCONFIG3], TUNE_SNR_THRESHOLD);/*VNVS:18-NOV'09---- modified for detecting more stations of good quality*/
	SYSCONFIG3_BITSET_SKCNT(&Si4709_dev.registers[SYSCONFIG3], TUNE_CNT_THRESHOLD);

        SYSCONFIG3_BITSET_RESERVED(&Si4709_dev.registers[SYSCONFIG3]);

        Si4709_dev.settings.timeout_RDS = msecs_to_jiffies(value);
	Si4709_dev.settings.curr_snr = TUNE_SNR_THRESHOLD;
	Si4709_dev.settings.curr_rssi_th = TUNE_RSSI_THRESHOLD;

        /*this will write all the above registers*/ 
        if( (ret = i2c_write(SYSCONFIG3)) < 0 )
        {
            debug("Si4709_dev_powerup i2c_write 1 failed");
        }
        else
        {
             Si4709_dev.valid = eTRUE;
        }

    }
	}
	else
		debug("Device already Powered-ON");

    mutex_unlock(&(Si4709_dev.lock)); 

    return ret;
}


int Si4709_dev_powerdown(void)
{
    int ret = 0;

    msleep(500);	// For avoiding turned off pop noise
    debug("Si4709_dev_powerdown called");

    mutex_lock(&(Si4709_dev.lock)); 

    if( Si4709_dev.valid == eFALSE )
    {
        debug("Si4709_dev_powerdown called when DS is invalid");
	       ret = -1;	  
    }    
    else if ( ( ret = powerdown()) < 0 )
    {
        debug("powerdown failed");
    }

    mutex_unlock(&(Si4709_dev.lock)); 

    return ret;
}

int Si4709_dev_suspend(void)
{
    int ret = 0;
	 
    debug("Si4709_dev_suspend called");

    mutex_lock(&(Si4709_dev.lock)); 

    if( Si4709_dev.valid_client_state== eFALSE )
    {
        debug("Si4709_dev_suspend called when DS (state, client) is invalid");
	       ret = -1;	  
    }    
#if 0
    else if( Si4709_dev.state.power_state == RADIO_ON )
    {
        ret = powerdown();
    }
#endif
    
    mutex_unlock(&(Si4709_dev.lock)); 

    debug("Si4709_dev_enable call over");

    return ret;
}

int Si4709_dev_resume(void)
{
    int ret = 0;
	 
//    debug("Si4709_dev_resume called");

    mutex_lock(&(Si4709_dev.lock));   

    if( Si4709_dev.valid_client_state == eFALSE )
    {
        debug("Si4709_dev_resume called when DS (state, client) is invalid");
	       ret = -1;	  
    }    
#if 0
    else  if( Si4709_dev.state.power_state == RADIO_POWERDOWN )
    {
        if( (ret = powerup()) < 0 )
        {
            debug("powerup failed");
        }
    }
#endif
	 
    mutex_unlock(&(Si4709_dev.lock)); 

//    debug("Si4709_dev_disable call over");

    return ret;
}

int Si4709_dev_band_set(int band)
{
    int ret = 0;
    u16 sysconfig2 =0; 
    u16 prev_band = 0;
    u32 prev_bottom_of_band = 0;
   
    debug("Si4709_dev_band_set called");
 
    mutex_lock(&(Si4709_dev.lock));   
     sysconfig2 = Si4709_dev.registers[SYSCONFIG2];     
     prev_band = Si4709_dev.settings.band;
     prev_bottom_of_band = Si4709_dev.settings.bottom_of_band;
    if( Si4709_dev.valid == eFALSE )
    {
        debug("Si4709_dev_band_set called when DS is invalid");
	       ret = -1;	  
    }
    else
    {
        switch (band)
        {
            case BAND_87500_108000_kHz:
            	   SYSCONFIG2_BITSET_BAND_87p5_108_MHz(&Si4709_dev.registers[SYSCONFIG2]);
            	   Si4709_dev.settings.band = BAND_87500_108000_kHz;
            	   Si4709_dev.settings.bottom_of_band = FREQ_87500_kHz;
            	   break;

            case BAND_76000_108000_kHz:
                SYSCONFIG2_BITSET_BAND_76_108_MHz(&Si4709_dev.registers[SYSCONFIG2]);
                Si4709_dev.settings.band = BAND_76000_108000_kHz;
                Si4709_dev.settings.bottom_of_band = FREQ_76000_kHz;
                break;

            case BAND_76000_90000_kHz:	
                SYSCONFIG2_BITSET_BAND_76_90_MHz(&Si4709_dev.registers[SYSCONFIG2]);
                Si4709_dev.settings.band = BAND_76000_90000_kHz;
                Si4709_dev.settings.bottom_of_band = FREQ_76000_kHz;
                break;

            default:
            	   ret = -1;
        }

        if(ret == 0)
        {
            if( (ret = i2c_write(SYSCONFIG2)) < 0 )
            {
                debug("Si4709_dev_band_set i2c_write 1 failed");
                Si4709_dev.settings.band = prev_band;
                Si4709_dev.settings.bottom_of_band = prev_bottom_of_band;
                Si4709_dev.registers[SYSCONFIG2] = sysconfig2;
            }
        }
    }
    
    mutex_unlock(&(Si4709_dev.lock)); 
 
    return ret;
}

int Si4709_dev_ch_spacing_set(int ch_spacing)
{
    int ret = 0;
    u16 sysconfig2 = 0; 
    u16 prev_ch_spacing = 0;
    
    debug("Si4709_dev_ch_spacing_set called");										
  
    mutex_lock(&(Si4709_dev.lock));   
     sysconfig2 = Si4709_dev.registers[SYSCONFIG2]; 
     prev_ch_spacing = Si4709_dev.settings.channel_spacing;
    if( Si4709_dev.valid == eFALSE )
    {
        debug("Si4709_dev_ch_spacing_set called when DS is invalid");				
        ret = -1;   
    }
    else
    {
        switch (ch_spacing)
        {
            case CHAN_SPACING_200_kHz:
                SYSCONFIG2_BITSET_SPACE_200_KHz(&Si4709_dev.registers[SYSCONFIG2]);
                Si4709_dev.settings.channel_spacing = CHAN_SPACING_200_kHz;
                break;
 
            case CHAN_SPACING_100_kHz:
                SYSCONFIG2_BITSET_SPACE_100_KHz(&Si4709_dev.registers[SYSCONFIG2]);
                Si4709_dev.settings.channel_spacing = CHAN_SPACING_100_kHz;
                break;
 
            case CHAN_SPACING_50_kHz: 
                SYSCONFIG2_BITSET_SPACE_50_KHz(&Si4709_dev.registers[SYSCONFIG2]);
                Si4709_dev.settings.channel_spacing = CHAN_SPACING_50_kHz;
                break;
 
            default:
                ret = -1;
        }
 
        if(ret == 0)
        {
            if( (ret = i2c_write(SYSCONFIG2)) < 0 )
            {
                debug("Si4709_dev_ch_spacing_set i2c_write 1 failed");
                Si4709_dev.settings.channel_spacing = prev_ch_spacing;
                Si4709_dev.registers[SYSCONFIG2] = sysconfig2;
            }
			
        }
    }
    
    mutex_unlock(&(Si4709_dev.lock)); 
  
    return ret;
}

int Si4709_dev_chan_select(u32 frequency)
{
    int ret = 0;
  
    debug("Si4709_dev_chan_select called");
 
    mutex_lock(&(Si4709_dev.lock));   

    if( Si4709_dev.valid == eFALSE )
    {
        debug("Si4709_dev_chan_select called when DS is invalid");
	       ret = -1;	  
    }
    else
    {
        Si4709_dev.state.seek_state = RADIO_SEEK_ON; 
        
        ret = tune_freq(frequency);
        debug("Si4709_dev_chan_select called1");
        Si4709_dev.state.seek_state = RADIO_SEEK_OFF; 
    }

    mutex_unlock(&(Si4709_dev.lock)); 
 
    return ret;
}

int Si4709_dev_chan_get(u32 *frequency)
{
    int ret = 0;

    
    debug("Si4709_dev_chan_get called");
  
    mutex_lock(&(Si4709_dev.lock));   

    if( Si4709_dev.valid == eFALSE )
    {
        debug("Si4709_dev_chan_get called when DS is invalid");
	       ret = -1;	  
    }
    else if( (ret = i2c_read(READCHAN)) < 0 )
    {
        debug("Si4709_dev_chan_get i2c_read failed");
    }
    else
    {
        get_cur_chan_freq(frequency, Si4709_dev.registers[READCHAN]);
    }
    
    mutex_unlock(&(Si4709_dev.lock)); 
  
    return ret;
}

int Si4709_dev_seek_up(u32 *frequency)
{
    int ret = 0;
   
    debug("Si4709_dev_seek_up called");
   
    mutex_lock(&(Si4709_dev.lock)); 

    if( Si4709_dev.valid == eFALSE )
    {
        debug("Si4709_dev_seek_up called when DS is invalid");
	       ret = -1;	  
    }
    else
    {
        Si4709_dev.state.seek_state = RADIO_SEEK_ON; 
        
        ret = seek(frequency, 1);

        Si4709_dev.state.seek_state = RADIO_SEEK_OFF; 
    }

    mutex_unlock(&(Si4709_dev.lock)); 
   
    return ret;
}

int Si4709_dev_seek_down(u32 *frequency)
{
    int ret = 0;
   
    debug("Si4709_dev_seek_down called");
   
    mutex_lock(&(Si4709_dev.lock));   

    if( Si4709_dev.valid == eFALSE )
    {
        debug("Si4709_dev_seek_down called when DS is invalid");
	       ret = -1;	  
    }
    else
    {
        Si4709_dev.state.seek_state = RADIO_SEEK_ON;   

        ret = seek(frequency, 0);
 
        Si4709_dev.state.seek_state = RADIO_SEEK_OFF;
    }

    mutex_unlock(&(Si4709_dev.lock)); 
   
    return ret;
}


#if 0
int Si4709_dev_seek_auto(u32 *seek_preset_user)
{
   u8 *rssi_seek;
   int ret = 0;
   int i =0;
   int j = 0;
   channel_into_t temp;

     debug("Si4709_dev_seek_auto called");
     
     if( Si4709_dev.valid == eFALSE )
       {
            debug("Si4709_dev_seek_auto called when DS is invalid");
	          ret = -1;	  
        }
    
    else if( (rssi_seek = (u8 *)kzalloc(sizeof(u8) * NUM_SEEK_PRESETS, GFP_KERNEL)) == NULL )
    {
         debug("Si4709_ioctl: no memory");
         ret = -ENOMEM;
    } 
               
    else
    	  {
    	        
       	      if( (ret = tune_freq(FREQ_87500_kHz)) == 0 )
                   {
                        debug("Si4709_dev_seek_auto tune_freq success");
                        get_cur_chan_freq(&(Si4709_dev.rssi_freq[0].frequency), Si4709_dev.registers[READCHAN]);
                        Si4709_dev_cur_RSSI_get(&(Si4709_dev.rssi_freq[0].rsssi_val));
                       
                    } 
       	      else
       	      	{
       	      	    debug("tunning failed, seek auto failed");
       	      	    ret =-1;
       	      	}
   #if 0    	    
              for(i=0;i<50; i++)
              	{       
                    	    if( (ret = seek(&(Si4709_dev.settings.seek_preset[i]),1)) == 0 )
                    	    	{
                    	    	     get_cur_chan_freq(&(Si4709_dev.rssi_freq[i].frequency), Si4709_dev.registers[READCHAN]);
									Si4709_dev_cur_RSSI_get(&(Si4709_dev.rssi_freq[i].rsssi_val));
									rssi_seek ++;
                           	}
                    	    
                    	    else
                    	      	{
                    	    	     debug("seek failed");
                    	    	    
                    	      	}
              	}
     #endif         
       
    

    /***new method ****/
                for(i=1;i<30; i++)
              	{       
                    	    if( (ret = seek(&(Si4709_dev.settings.seek_preset[i]),1)) == 0 )
                    	    	{
                    	    	     get_cur_chan_freq(&(Si4709_dev.rssi_freq[i].frequency), Si4709_dev.registers[READCHAN]);
                               Si4709_dev_cur_RSSI_get(&(Si4709_dev.rssi_freq[i].rsssi_val));
                           	}
                    	    
                    	    else
                    	      	{
                    	    	     debug("seek failed");
                    	    	    
                    	      	}
              	}
             /***Sort the array of structures on the basis of RSSI value****/
             for(i=0;i<29;i++)
                {
                
                     for(j=i+1;j<30;j++)
                        {
                        
                            if(  Si4709_dev.rssi_freq[j].rsssi_val>Si4709_dev.rssi_freq[i].rsssi_val)
                            	{
                            	    temp=Si4709_dev.rssi_freq[i];
                            	    Si4709_dev.rssi_freq[i]=Si4709_dev.rssi_freq[j];
                            	    Si4709_dev.rssi_freq[j]=temp;
                            	}
                        
                     	 }

               	  }

             /***Store the frequency in Array*****/
             for(i=0;i<19;i++)
             	{
             	     Si4709_dev.settings.seek_preset[i]=Si4709_dev.rssi_freq[i].frequency;
             	}

                
     
    	}
    memcpy(seek_preset_user, Si4709_dev.settings.seek_preset , sizeof(int)*NUM_SEEK_PRESETS);
    kfree(rssi_seek);
    return ret;
}
#endif



int Si4709_dev_RSSI_seek_th_set(u8 seek_th)
{
    int ret = 0;
    u16 sysconfig2 = 0;
       
    debug("Si4709_dev_RSSI_seek_th_set called");
     
    mutex_lock(&(Si4709_dev.lock));   
    sysconfig2 = Si4709_dev.registers[SYSCONFIG2];
    
    if( Si4709_dev.valid == eFALSE )
    {
        debug("Si4709_dev_RSSI_seek_th_set called when DS is invalid");
	       ret = -1;	  
    }
    else
    {
        SYSCONFIG2_BITSET_SEEKTH(&Si4709_dev.registers[SYSCONFIG2], seek_th);
        Si4709_dev.settings.curr_rssi_th=seek_th;
        if( (ret = i2c_write( SYSCONFIG2 )) < 0 )
        {
            debug("Si4709_dev_RSSI_seek_th_set i2c_write 1 failed");
            Si4709_dev.registers[SYSCONFIG2] = sysconfig2;
        }
    }
    
    mutex_unlock(&(Si4709_dev.lock)); 
     
    return ret;
}

int Si4709_dev_seek_SNR_th_set(u8 seek_SNR)
{
    int ret = 0;
    u16 sysconfig3 = 0;
        
    debug("Si4709_dev_seek_SNR_th_set called");
      
    mutex_lock(&(Si4709_dev.lock));   
    
    sysconfig3 = Si4709_dev.registers[SYSCONFIG3]; 
    
    if( Si4709_dev.valid == eFALSE )
    {
        debug("Si4709_dev_seek_SNR_th_set called when DS is invalid");
	       ret = -1;	  
    }
    else
    {
        SYSCONFIG3_BITSET_SKSNR(&Si4709_dev.registers[SYSCONFIG3], seek_SNR);
        SYSCONFIG3_BITSET_RESERVED(&Si4709_dev.registers[SYSCONFIG3]);
        Si4709_dev.settings.curr_snr=seek_SNR;

        if( (ret = i2c_write( SYSCONFIG3 )) < 0 )
        {
           debug("Si4709_dev_seek_SNR_th_set i2c_write 1 failed");
           Si4709_dev.registers[SYSCONFIG3] = sysconfig3;
        }
    }
    
    mutex_unlock(&(Si4709_dev.lock)); 
      
    return ret;
}

int Si4709_dev_seek_FM_ID_th_set(u8 seek_FM_ID_th)
{
    int ret = 0;
    u16 sysconfig3 = 0;
        
    debug("Si4709_dev_seek_FM_ID_th_set called");
       
    mutex_lock(&(Si4709_dev.lock));   
    
    sysconfig3 = Si4709_dev.registers[SYSCONFIG3];
    
    if( Si4709_dev.valid == eFALSE )
    {
        debug("Si4709_dev_seek_SNR_th_set called when DS is invalid");
	       ret = -1;	  
    }
    else
    {
        SYSCONFIG3_BITSET_SKCNT(&Si4709_dev.registers[SYSCONFIG3], seek_FM_ID_th);
        SYSCONFIG3_BITSET_RESERVED(&Si4709_dev.registers[SYSCONFIG3]);

        if( (ret = i2c_write( SYSCONFIG3 )) < 0 )
        {
            debug("Si4709_dev_seek_FM_ID_th_set i2c_write 1 failed");
            sysconfig3 = Si4709_dev.registers[SYSCONFIG3];
        }
    }       
        
    mutex_unlock(&(Si4709_dev.lock)); 
       
    return ret;
}

int Si4709_dev_cur_RSSI_get(rssi_snr_t *cur_RSSI)
{
    int ret = 0;
         
    debug("Si4709_dev_cur_RSSI_get called");
        
    mutex_lock(&(Si4709_dev.lock));   
   
    if( Si4709_dev.valid == eFALSE )
    {
        debug("Si4709_dev_cur_RSSI_get called when DS is invalid");
	       ret = -1;	  
    }
    else
    {
        if( (ret = i2c_read(STATUSRSSI)) < 0 )
        {
            debug("Si4709_dev_cur_RSSI_get i2c_read 1 failed");
        }
        else
        {
              
            cur_RSSI->curr_rssi= STATUSRSSI_RSSI_SIGNAL_STRENGTH(Si4709_dev.registers[STATUSRSSI]);
            cur_RSSI->curr_rssi_th=Si4709_dev.settings.curr_rssi_th;
             cur_RSSI->curr_snr=Si4709_dev.settings.curr_snr;
        }
    }     
    mutex_unlock(&(Si4709_dev.lock)); 
        
    return ret;
}											

/*VNVS:START 13-OCT'09---- Functions which reads device-id,chip-id,power configuration, system configuration2 registers */	
int Si4709_dev_device_id(device_id *dev_id)												
{		
	int ret = 0;
         
    debug("Si4709_dev_device_id called");
        
    mutex_lock(&(Si4709_dev.lock));   
   
    if( Si4709_dev.valid == eFALSE )
    {
        debug("Si4709_dev_device_id called when DS is invalid");
	       ret = -1;	  
    }
    else
    {
        if( (ret = i2c_read(DEVICE_ID)) < 0 )
        {
            debug("Si4709_dev_device_id i2c_read failed");
        }
        else
        {
              
            dev_id->part_number= DEVICE_ID_PART_NUMBER(Si4709_dev.registers[DEVICE_ID]);
			dev_id->manufact_number =  DEVICE_ID_MANUFACT_NUMBER(Si4709_dev.registers[DEVICE_ID]);
            
        }
    }     
    mutex_unlock(&(Si4709_dev.lock)); 
        
    return ret;
}																					

int Si4709_dev_chip_id(chip_id *chp_id)												
{		
	int ret = 0;
         
    debug("Si4709_dev_chip_id called");
        
    mutex_lock(&(Si4709_dev.lock));   
   
    if( Si4709_dev.valid == eFALSE )
    {
        debug("Si4709_dev_chip_id called when DS is invalid");
	       ret = -1;	  
    }
    else
    {
        if( (ret = i2c_read(CHIP_ID)) < 0 )
        {
            debug("Si4709_dev_chip_id i2c_read failed");
        }
        else
        {
              
            chp_id->chip_version= CHIP_ID_CHIP_VERSION(Si4709_dev.registers[CHIP_ID]);
			chp_id->device =  CHIP_ID_DEVICE(Si4709_dev.registers[CHIP_ID]);
            chp_id->firmware_version = CHIP_ID_FIRMWARE_VERSION(Si4709_dev.registers[CHIP_ID]);
        }
    }     
    mutex_unlock(&(Si4709_dev.lock)); 
        
    return ret;
}                                                                                         

int Si4709_dev_sys_config2(sys_config2 *sys_conf2)
{
	int ret = 0;
         
	debug("Si4709_sys_config2 called");
        
	mutex_lock(&(Si4709_dev.lock));   
   
	if( Si4709_dev.valid == eFALSE )
	{
		debug("Si4709_sys_config2 called when DS is invalid");
		ret = -1;	  
	}
	else
	{
		if((ret = i2c_read(SYSCONFIG2)) < 0)
		{
			debug("Si4709_sys_config2 i2c_read failed");
		}
		else
		{
			sys_conf2->rssi_th =SYS_CONFIG2_RSSI_TH(Si4709_dev.registers[SYSCONFIG2]);
			sys_conf2->fm_band = SYS_CONFIG2_FM_BAND(Si4709_dev.registers[SYSCONFIG2]);
			sys_conf2->fm_chan_spac = SYS_CONFIG2_FM_CHAN_SPAC(Si4709_dev.registers[SYSCONFIG2]);
			sys_conf2->fm_vol = SYS_CONFIG2_FM_VOL(Si4709_dev.registers[SYSCONFIG2]);
		}
	}     
	mutex_unlock(&(Si4709_dev.lock)); 
        
	return ret;
}                                                                                              

int Si4709_dev_sys_config3(sys_config3 *sys_conf3)
{
	int ret = 0;
         
	debug("Si4709_sys_config3 called");
        
	mutex_lock(&(Si4709_dev.lock));   
   
	if( Si4709_dev.valid == eFALSE )
	{
		debug("Si4709_sys_config3 called when DS is invalid");
		ret = -1;	  
	}
	else
	{
		if((ret = i2c_read(SYSCONFIG3)) < 0)
		{
			debug("Si4709_sys_config3 i2c_read failed");
		}
		else
		{
			sys_conf3->smmute = (Si4709_dev.registers[SYSCONFIG3] & 0xC000) >> 14;
			sys_conf3->smutea = (Si4709_dev.registers[SYSCONFIG3] & 0x3000) >> 12;
			sys_conf3->volext = (Si4709_dev.registers[SYSCONFIG3] & 0x0100) >> 8;
			sys_conf3->sksnr = (Si4709_dev.registers[SYSCONFIG3] & 0x00F0) >> 4;
			sys_conf3->skcnt = (Si4709_dev.registers[SYSCONFIG3] & 0x000F);
		}
	}     
	mutex_unlock(&(Si4709_dev.lock)); 
        
	return ret;
}                                                                                              

int Si4709_dev_status_rssi(status_rssi *status)
{
	int ret = 0;
         
	debug("Si4709_dev_status_rssi called");
        
	mutex_lock(&(Si4709_dev.lock));   
   
	if( Si4709_dev.valid == eFALSE )
	{
		debug("Si4709_dev_status_rssi called when DS is invalid");
		ret = -1;	  
	}
	else
	{
		if((ret = i2c_read(STATUSRSSI)) < 0)
		{
			debug("Si4709_sys_config3 i2c_read failed");
		}
		else
		{
			status->rdsr = (Si4709_dev.registers[STATUSRSSI] & 0x8000) >> 15;
			status->stc = (Si4709_dev.registers[STATUSRSSI] & 0x4000) >> 14;
			status->sfbl = (Si4709_dev.registers[STATUSRSSI] & 0x2000) >> 13;
			status->afcrl = (Si4709_dev.registers[STATUSRSSI] & 0x1000) >> 12;
			status->rdss = (Si4709_dev.registers[STATUSRSSI] & 0x0800) >> 11;
			status->blera = (Si4709_dev.registers[STATUSRSSI] & 0x0600) >> 9; 
			status->st = (Si4709_dev.registers[STATUSRSSI] & 0x0100) >> 8;
			status->rssi = (Si4709_dev.registers[STATUSRSSI] & 0x00FF);
		}
	}     
	mutex_unlock(&(Si4709_dev.lock)); 
        
	return ret;
}                                                                                              

int Si4709_dev_sys_config2_set(sys_config2 *sys_conf2)
{
	int ret = 0;
	u16 register_bak = 0;
         
	debug("Si4709_dev_sys_config2_set called");
        
	mutex_lock(&(Si4709_dev.lock));   

	register_bak = Si4709_dev.registers[SYSCONFIG2];
    
	if( Si4709_dev.valid == eFALSE )
	{
		debug("Si4709_dev_sys_config2_set called when DS is invalid");
		ret = -1;
	}
	else
	{
		printk(KERN_ERR "Si4709_dev_sys_config2_set() : Register Value = [0x%X], rssi-th = [%X]\n", Si4709_dev.registers[SYSCONFIG2], sys_conf2->rssi_th);
		Si4709_dev.registers[SYSCONFIG2] = (Si4709_dev.registers[SYSCONFIG2] & 0x00FF) | ((sys_conf2->rssi_th) << 8);
		Si4709_dev.registers[SYSCONFIG2] = (Si4709_dev.registers[SYSCONFIG2] & 0xFF3F) | ((sys_conf2->fm_band) << 6);
		Si4709_dev.registers[SYSCONFIG2] = (Si4709_dev.registers[SYSCONFIG2] & 0xFFCF) | ((sys_conf2->fm_chan_spac) << 4);
		Si4709_dev.registers[SYSCONFIG2] = (Si4709_dev.registers[SYSCONFIG2] & 0xFFF0) | (sys_conf2->fm_vol);
		printk(KERN_ERR "Si4709_dev_sys_config2_set() : After Register Value = [0x%X]\n", Si4709_dev.registers[SYSCONFIG2]);
			
		if( (ret = i2c_write( SYSCONFIG2 )) < 0 )
		{
			debug("Si4709_dev_sys_config2_set i2c_write 1 failed");
			Si4709_dev.registers[SYSCONFIG2] = register_bak;
		}
		else
			printk(KERN_ERR "Si4709_dev_sys_config2_set() : Write Sucess!!");	
	}
	
	mutex_unlock(&(Si4709_dev.lock)); 
        
	return ret;
}                                                                                              

int Si4709_dev_sys_config3_set(sys_config3 *sys_conf3)
{
	int ret = 0;
	u16 register_bak = 0;
         
	debug("Si4709_dev_sys_config3_set called");
        
	mutex_lock(&(Si4709_dev.lock));   

	register_bak = Si4709_dev.registers[SYSCONFIG3];
    
	if( Si4709_dev.valid == eFALSE )
	{
		debug("Si4709_dev_sys_config3_set called when DS is invalid");
		ret = -1;
	}
	else
	{
		printk(KERN_ERR "Si4709_dev_sys_config3_set() : Register Value = [0x%X], sksnrth = [%X]\n", Si4709_dev.registers[SYSCONFIG3], sys_conf3->sksnr);
		Si4709_dev.registers[SYSCONFIG3] = (Si4709_dev.registers[SYSCONFIG3] & 0x3FFF) | ((sys_conf3->smmute) << 14);
		Si4709_dev.registers[SYSCONFIG3] = (Si4709_dev.registers[SYSCONFIG3] & 0xCFFF) | ((sys_conf3->smutea) << 12);
		Si4709_dev.registers[SYSCONFIG3] = (Si4709_dev.registers[SYSCONFIG3] & 0xFEFF) | ((sys_conf3->volext) << 8);
		Si4709_dev.registers[SYSCONFIG3] = (Si4709_dev.registers[SYSCONFIG3] & 0xFF0F) | ((sys_conf3->sksnr) << 4);
		Si4709_dev.registers[SYSCONFIG3] = (Si4709_dev.registers[SYSCONFIG3] & 0xFFF0) | (sys_conf3->skcnt);
		printk(KERN_ERR "Si4709_dev_sys_config3_set() : After Register Value = [0x%X]\n", Si4709_dev.registers[SYSCONFIG3]);
			
		if((ret = i2c_write( SYSCONFIG3)) < 0)
		{
			debug("Si4709_dev_sys_config3_set i2c_write 1 failed");
			Si4709_dev.registers[SYSCONFIG3] = register_bak;
		}
	}
	
	mutex_unlock(&(Si4709_dev.lock)); 
        
	return ret;
}                                                                                              

int Si4709_dev_power_config(power_config *pow_conf)											   
{
	int ret =0;
	
	debug("Si4709_dev_power_config called");
	mutex_lock(&(Si4709_dev.lock));   
   
    if( Si4709_dev.valid == eFALSE )
    {
        debug("Si4709_dev_power_config called when DS is invalid");
	    ret = -1;	  
    }
    else
    {
        if( (ret = i2c_read(POWERCFG)) < 0 )
        {
            debug("Si4709_dev_power_config i2c_read failed");
        }
        else
        {
              
            pow_conf->dsmute		=POWER_CONFIG_SOFTMUTE_STATUS(Si4709_dev.registers[POWERCFG]);
			pow_conf->dmute 		=POWER_CONFIG_MUTE_STATUS(Si4709_dev.registers[POWERCFG]);
            pow_conf->mono 			=POWER_CONFIG_MONO_STATUS(Si4709_dev.registers[POWERCFG]);
			pow_conf->rds_mode		=POWER_CONFIG_RDS_MODE_STATUS(Si4709_dev.registers[POWERCFG]);
			pow_conf->sk_mode		=POWER_CONFIG_SKMODE_STATUS(Si4709_dev.registers[POWERCFG]);
			pow_conf->seek_up		=POWER_CONFIG_SEEKUP_STATUS(Si4709_dev.registers[POWERCFG]);
			pow_conf->seek			=POWER_CONFIG_SEEK_STATUS(Si4709_dev.registers[POWERCFG]);
			pow_conf->power_disable	=POWER_CONFIG_DISABLE_STATUS(Si4709_dev.registers[POWERCFG]);
			pow_conf->power_enable	=POWER_CONFIG_ENABLE_STATUS(Si4709_dev.registers[POWERCFG]);
        }
    }     
    mutex_unlock(&(Si4709_dev.lock)); 
        
    return ret;
}            					
/*VNVS:END*/

/*VNVS:START 18-NOV'09*/
/*Reading AFCRL Bit*/
int Si4709_dev_AFCRL_get(u8 *afc)
{
    int ret = 0;

    debug("Si4709_dev_AFCRL_get called");
            
    mutex_lock(&(Si4709_dev.lock));   

    if( Si4709_dev.valid == eFALSE )
    {
         debug("Si4709_dev_AFCRL_get called when DS is invalid");
         ret = -1;   
    }
    else
    {
		if( (ret = i2c_read(STATUSRSSI)) < 0 )
        {
            debug("Si4709_dev_AFCRL_get i2c_read failed");
        }
        *afc = STATUSRSSI_AFC_RAIL_STATUS(Si4709_dev.registers[STATUSRSSI]);
    }            
             
    mutex_unlock(&(Si4709_dev.lock)); 
            
    return ret;
}
/*Setting DE-emphasis time constant 50us(Europe,Japan,Australia) or 75us(USA)*/
int Si4709_dev_DE_set(u8 de_tc)
{
    u16 sysconfig1 = 0;
    int ret = 0;
             
    debug("Si4709_dev_DE_set called");
            
    mutex_lock(&(Si4709_dev.lock));   
    
    sysconfig1 = Si4709_dev.registers[SYSCONFIG1];
    
    if( Si4709_dev.valid == eFALSE )
    {
         debug("Si4709_dev_DE_set called when DS is invalid");
         ret = -1;   
    }
    else
    {
     	 switch(de_tc)
		 {		 
			case DE_TIME_CONSTANT_50:
				SYSCONFIG1_BITSET_DE_50(&Si4709_dev.registers[SYSCONFIG1]);
				SYSCONFIG1_BITSET_RESERVED( &Si4709_dev.registers[SYSCONFIG1] );
				break;
			case DE_TIME_CONSTANT_75:
				SYSCONFIG1_BITSET_DE_75(&Si4709_dev.registers[SYSCONFIG1]);
				SYSCONFIG1_BITSET_RESERVED( &Si4709_dev.registers[SYSCONFIG1] );
				break;
			default:
                ret = -1;
		 }
    	 
		 if(0==ret)
			if( (ret = i2c_write(SYSCONFIG1)) < 0 )
			{
				debug("Si4709_dev_DE_set i2c_write failed");
				Si4709_dev.registers[SYSCONFIG1] = sysconfig1;
			}
   	} 	   

    mutex_unlock(&(Si4709_dev.lock)); 

    return ret;
}
/*VNVS:END*/
int Si4709_dev_VOLEXT_ENB(void)
{
    int ret = 0;
    u16 sysconfig3 = 0;
          
    debug("Si4709_dev_VOLEXT_ENB called");
         
    mutex_lock(&(Si4709_dev.lock)); 
    
    sysconfig3 = Si4709_dev.registers[SYSCONFIG3];    
     
    if( Si4709_dev.valid == eFALSE )
    {
        debug("Si4709_dev_VOLEXT_ENB called when DS is invalid");
        ret = -1;   
    }
    else
    {
        SYSCONFIG3_BITSET_VOLEXT_ENB(&Si4709_dev.registers[SYSCONFIG3]);
        SYSCONFIG3_BITSET_RESERVED(&Si4709_dev.registers[SYSCONFIG3]);

        if( (ret = i2c_write( SYSCONFIG3 )) < 0 )
        {
            debug("Si4709_dev_VOLEXT_ENB i2c_write failed");
            Si4709_dev.registers[SYSCONFIG3] = sysconfig3;
        }
    }
          
    mutex_unlock(&(Si4709_dev.lock)); 
         
    return ret;
}

int Si4709_dev_VOLEXT_DISB(void)
{
    int ret = 0;
    u16 sysconfig3 = 0;
           
    debug("Si4709_dev_VOLEXT_DISB called");
          
    mutex_lock(&(Si4709_dev.lock));  
    
    sysconfig3 = Si4709_dev.registers[SYSCONFIG3];    
    
    if( Si4709_dev.valid == eFALSE )
    {
         debug("Si4709_dev_VOLEXT_DISB called when DS is invalid");
         ret = -1;   
    }
    else
    {
         SYSCONFIG3_BITSET_VOLEXT_DISB(&Si4709_dev.registers[SYSCONFIG3]);
         SYSCONFIG3_BITSET_RESERVED(&Si4709_dev.registers[SYSCONFIG3]);
       
         if( (ret = i2c_write( SYSCONFIG3 )) < 0 )
         {
             debug("Si4709_dev_VOLEXT_DISB i2c_write failed");
             Si4709_dev.registers[SYSCONFIG3] = sysconfig3;
         }
    }
          
    mutex_unlock(&(Si4709_dev.lock)); 
          
    return ret;
}

int Si4709_dev_volume_set(u8 volume)
{
    int ret = 0;
    u16 sysconfig2 = 0;
            
    debug("Si4709_dev_volume_set called");
           
    mutex_lock(&(Si4709_dev.lock));   
    
    sysconfig2 = Si4709_dev.registers[SYSCONFIG2];       
    
    if( Si4709_dev.valid == eFALSE )
    {
         debug("Si4709_dev_volume_set called when DS is invalid");
         ret = -1;   
    }
    else
    {
         SYSCONFIG2_BITSET_VOLUME(&Si4709_dev.registers[SYSCONFIG2], volume);
         
         if( (ret = i2c_write( SYSCONFIG2 )) < 0 )
         {
             debug("Si4709_dev_volume_set i2c_write failed");
             Si4709_dev.registers[SYSCONFIG2] = sysconfig2;
         }
    }
    
    mutex_unlock(&(Si4709_dev.lock)); 
           
    return ret;
}

int Si4709_dev_volume_get(u8 *volume)
{
    int ret = 0;

    debug("Si4709_dev_volume_get called");
            
    mutex_lock(&(Si4709_dev.lock));   

    if( Si4709_dev.valid == eFALSE )
    {
         debug("Si4709_dev_volume_get called when DS is invalid");
         ret = -1;   
    }
    else
    {
         *volume = SYSCONFIG2_GET_VOLUME(Si4709_dev.registers[SYSCONFIG2]);
    }            
             
    mutex_unlock(&(Si4709_dev.lock)); 
            
    return ret;
}

int Si4709_dev_MUTE_ON(void)
{
    int ret = 0;
    u16 powercfg = 0;
              
    debug("Si4709_dev_MUTE_ON called");
             
    mutex_lock(&(Si4709_dev.lock));   
    powercfg = Si4709_dev.registers[POWERCFG];
    if( Si4709_dev.valid == eFALSE )
    {
         debug("Si4709_dev_MUTE_ON called when DS is invalid");
         ret = -1;   
    }
    else
    {
         POWERCFG_BITSET_DMUTE_LOW(&Si4709_dev.registers[POWERCFG]);
         POWERCFG_BITSET_RESERVED(&Si4709_dev.registers[POWERCFG]);

         if( (ret = i2c_write( POWERCFG )) < 0 )
         {
             debug("Si4709_dev_MUTE_ON i2c_write failed");
             Si4709_dev.registers[POWERCFG] = powercfg;
         }
    }                   
              
    mutex_unlock(&(Si4709_dev.lock)); 
             
    return ret;
}

int Si4709_dev_MUTE_OFF(void)
{
    int ret = 0;
    u16 powercfg = 0;
              
    debug("Si4709_dev_MUTE_OFF called");
              
    mutex_lock(&(Si4709_dev.lock));   
    
    powercfg = Si4709_dev.registers[POWERCFG];
    
    if( Si4709_dev.valid == eFALSE )
    {
         debug("Si4709_dev_MUTE_OFF called when DS is invalid");
         ret = -1;   
    }
    else
    {
         POWERCFG_BITSET_DMUTE_HIGH(&Si4709_dev.registers[POWERCFG]);
         POWERCFG_BITSET_RESERVED(&Si4709_dev.registers[POWERCFG]);
 
         if( (ret = i2c_write( POWERCFG )) < 0 )
         {
             debug("Si4709_dev_MUTE_OFF i2c_write failed");
             Si4709_dev.registers[POWERCFG] = powercfg;
         }
    }                   
               
    mutex_unlock(&(Si4709_dev.lock)); 
              
    return ret;
}

int Si4709_dev_MONO_SET(void)
{
    int ret = 0;
    u16 powercfg = 0;
               
    debug("Si4709_dev_MONO_SET called");
               
    mutex_lock(&(Si4709_dev.lock)); 
    
    powercfg = Si4709_dev.registers[POWERCFG];
    
    if( Si4709_dev.valid == eFALSE )
    {
        debug("Si4709_dev_MONO_SET called when DS is invalid");
        ret = -1;   
    }
    else
    {
        POWERCFG_BITSET_MONO_HIGH(&Si4709_dev.registers[POWERCFG]);
        POWERCFG_BITSET_RESERVED(&Si4709_dev.registers[POWERCFG]);
 
        if( (ret = i2c_write( POWERCFG )) < 0 )
        {
            debug("Si4709_dev_MONO_SET i2c_write failed");
            Si4709_dev.registers[POWERCFG] = powercfg;
        }
    }                   
                
    mutex_unlock(&(Si4709_dev.lock)); 
              
    return ret;
}

int Si4709_dev_STEREO_SET(void)
{
    int ret = 0;
    u16 powercfg = 0;
                
    debug("Si4709_dev_STEREO_SET called");
                
    mutex_lock(&(Si4709_dev.lock));   
    
    powercfg = Si4709_dev.registers[POWERCFG];
    
    if( Si4709_dev.valid == eFALSE )
    {
        debug("Si4709_dev_STEREO_SET called when DS is invalid");
        ret = -1;   
    }
    else
    {
        POWERCFG_BITSET_MONO_LOW(&Si4709_dev.registers[POWERCFG]);
        POWERCFG_BITSET_RESERVED(&Si4709_dev.registers[POWERCFG]);
  
        if( (ret = i2c_write( POWERCFG )) < 0 )
        {
            debug("Si4709_dev_STEREO_SET i2c_write failed");
            Si4709_dev.registers[POWERCFG] = powercfg; 
        }
    }                   
                
    mutex_unlock(&(Si4709_dev.lock)); 
               
    return ret;
}

int Si4709_dev_RDS_ENABLE(void)
{
    u16 sysconfig1 = 0;
    int ret = 0;
             
    debug("Si4709_dev_RDS_ENABLE called");
            
    mutex_lock(&(Si4709_dev.lock));   
    
    sysconfig1 = Si4709_dev.registers[SYSCONFIG1];
    
    if( Si4709_dev.valid == eFALSE )
    {
         debug("Si4709_dev_RDS_ENABLE called when DS is invalid");
         ret = -1;   
    }
    else
    {
     	   SYSCONFIG1_BITSET_RDS_HIGH(&Si4709_dev.registers[SYSCONFIG1]);
		   SYSCONFIG1_BITSET_RESERVED( &Si4709_dev.registers[SYSCONFIG1] );	
    	    if( (ret = i2c_write(SYSCONFIG1)) < 0 )
         {
            debug("Si4709_dev_RDS_ENABLE i2c_write failed");
            Si4709_dev.registers[SYSCONFIG1] = sysconfig1;
         }
   	} 	   

    mutex_unlock(&(Si4709_dev.lock)); 
   
    return ret;
}



int Si4709_dev_RDS_DISABLE(void)
{
    u16 sysconfig1 = 0;
    int ret = 0;
             
    debug("Si4709_dev_RDS_DISABLE called");
            
    mutex_lock(&(Si4709_dev.lock));   
    
    sysconfig1 = Si4709_dev.registers[SYSCONFIG1];
    
    if( Si4709_dev.valid == eFALSE )
    {
         debug("Si4709_dev_RDS_DISABLE called when DS is invalid");
         ret = -1;   
    }
    else
    {
     	   SYSCONFIG1_BITSET_RDS_LOW(&Si4709_dev.registers[SYSCONFIG1]);
		   SYSCONFIG1_BITSET_RESERVED( &Si4709_dev.registers[SYSCONFIG1] );
         if( (ret = i2c_write(SYSCONFIG1)) < 0 )
         {
             debug("Si4709_dev_RDS_DISABLE i2c_write failed");
             Si4709_dev.registers[SYSCONFIG1] = sysconfig1;
         }
   	} 	   

    mutex_unlock(&(Si4709_dev.lock)); 

    return ret;
}
     

int Si4709_dev_rstate_get(dev_state_t *dev_state)
{
    int ret = 0;
                
    debug("Si4709_dev_rstate_get called");
                
    mutex_lock(&(Si4709_dev.lock));   

    if( Si4709_dev.valid == eFALSE )
    {
        debug("Si4709_dev_rstate_get called when DS is invalid");
        ret = -1;   
    }
    else
    {
        dev_state->power_state = Si4709_dev.state.power_state;
        dev_state->seek_state = Si4709_dev.state.seek_state;
    }             

    mutex_unlock(&(Si4709_dev.lock)); 
                
    return ret;
}

int Si4709_dev_RDS_data_get(radio_data_t *data)
{
	int ret = 0;
	u16 sysconfig1 = 0;

	debug("Si4709_dev_RDS_data_get called");

	mutex_lock(&(Si4709_dev.lock));  
    
	sysconfig1 = Si4709_dev.registers[SYSCONFIG1];
    
	if( Si4709_dev.valid == eFALSE )
	{
		debug("Si4709_dev_RDS_data_get called when DS is invalid");
		ret = -1;
	}
	else
	{
		SYSCONFIG1_BITSET_RDSIEN_HIGH(&Si4709_dev.registers[SYSCONFIG1]);

		if((ret = i2c_write(SYSCONFIG1)) < 0)
		{
			debug("Si4709_dev_RDS_data_get i2c_write 1 failed");
			Si4709_dev.registers[SYSCONFIG1] = sysconfig1;
		}
		else	{
			if( (ret=i2c_read(SYSCONFIG1)) < 0);
				debug("Si4709_dev_RDS_data_get i2c_read 1 failed");

			debug("sysconfig1: 0x%x",Si4709_dev.registers[SYSCONFIG1] );
			sysconfig1 = Si4709_dev.registers[SYSCONFIG1];

			Si4709_dev_wait_flag = RDS_WAITING;
        
			wait_RDS();

			if((ret=i2c_read(STATUSRSSI)) < 0)
				debug("Si4709_dev_RDS_data_get i2c_read 2 failed");

			debug("statusrssi: 0x%x",Si4709_dev.registers[STATUSRSSI] );
			SYSCONFIG1_BITSET_RDSIEN_LOW(&Si4709_dev.registers[SYSCONFIG1]);

			if ((ret = i2c_write(SYSCONFIG1)) < 0)
			{
				debug("Si4709_dev_RDS_data_get i2c_write 2 failed");
				Si4709_dev.registers[SYSCONFIG1] = sysconfig1;
			}
            		else if(Si4709_dev_wait_flag == WAIT_OVER)
			{
				Si4709_dev_wait_flag = NO_WAIT;

				if((ret = i2c_read(RDSD)) < 0)
					debug("Si4709_dev_RDS_data_get i2c_read 3 failed");
				else 
				{
					data->rdsa = Si4709_dev.registers[RDSA];
					data->rdsb = Si4709_dev.registers[RDSB];
					data->rdsc = Si4709_dev.registers[RDSC];
					data->rdsd = Si4709_dev.registers[RDSD];

					get_cur_chan_freq(&(data->curr_channel), Si4709_dev.registers[READCHAN]);
					debug("curr_channel: %u",data->curr_channel);
					data->curr_rssi = STATUSRSSI_RSSI_SIGNAL_STRENGTH(Si4709_dev.registers[STATUSRSSI]);
					debug("curr_rssi:%u",(u32)data->curr_rssi);
					data->blera = STATUSRSSI_RDS_BLOCK_A_ERRORS(Si4709_dev.registers[STATUSRSSI]);
					data->blerb = READCHAN_BLOCK_B_ERRORS(Si4709_dev.registers[READCHAN]);
					data->blerc = READCHAN_BLOCK_C_ERRORS(Si4709_dev.registers[READCHAN]);
					data->blerd = READCHAN_BLOCK_D_ERRORS(Si4709_dev.registers[READCHAN]);
				}
			}
            		else
			{
				debug("Si4709_dev_RDS_data_get failure no interrupt or timeout");
				Si4709_dev_wait_flag = NO_WAIT;
				ret = -1;
			}
        	} 
	}

	mutex_unlock(&(Si4709_dev.lock)); 
                
	return ret;
}

int Si4709_dev_RDS_timeout_set(u32 time_out)
{
    int ret = 0; 
    u32 jiffy_count = 0;
    
    debug("Si4709_dev_RDS_timeout_set called");
    /****convert time_out(in milliseconds) into jiffies*****/
    
    jiffy_count = msecs_to_jiffies(time_out);

    debug("jiffy_count%d",jiffy_count);

    mutex_lock(&(Si4709_dev.lock));   
     
    if( Si4709_dev.valid == eFALSE )
    {
        debug("Si4709_dev_RDS_timeout_set called when DS is invalid");
        ret = -1;   
    }
    else
    {
        Si4709_dev.settings.timeout_RDS = jiffy_count;
    }

    mutex_unlock(&(Si4709_dev.lock));  

    return ret;
}
    

/**************************************************************/
static int powerup(void)
{
    int ret=0;
    u16 powercfg = Si4709_dev.registers[POWERCFG];
	int reg;
	 /****Resetting the device****/
	
	gpio_set_value(FM_RESET, GPIO_LEVEL_LOW);	
	gpio_set_value(FM_RESET, GPIO_LEVEL_HIGH);	
								
#if 0
    /*Add the i2c driver*/
    if ( (ret = Si4709_i2c_drv_init() < 0) ) 
    {
         debug("Si4709_i2c_drv_init failed");
    }
#endif
	/*Resetting the Si4709_dev.registers[] array*/
	for(reg=0;reg < NUM_OF_REGISTERS;reg++)
		Si4709_dev.registers[reg] = 0;
	
	debug("Resetting the Si4709_dev.registers[] array");

    POWERCFG_BITSET_DMUTE_HIGH( &Si4709_dev.registers[POWERCFG] );
    POWERCFG_BITSET_ENABLE_HIGH( &Si4709_dev.registers[POWERCFG] );
    POWERCFG_BITSET_DISABLE_LOW( &Si4709_dev.registers[POWERCFG] );
    POWERCFG_BITSET_RESERVED( &Si4709_dev.registers[POWERCFG] );
 
    if( (ret = i2c_write(POWERCFG)) < 0 )
    {
        debug("powerup->i2c_write 1 failed");
        Si4709_dev.registers[POWERCFG] = powercfg;
    }
    else
    {
        /*Si4709/09 datasheet: Table 7*/
        mdelay(110);
        Si4709_dev.state.power_state = RADIO_ON;
    }

    return ret;
}

static int powerdown(void)
{
    int ret = 0;
    u16 test1 = Si4709_dev.registers[TEST1], 
    	   sysconfig1 = Si4709_dev.registers[SYSCONFIG1],
    	   powercfg = Si4709_dev.registers[POWERCFG];

	if(!(RADIO_POWERDOWN==Si4709_dev.state.power_state))
	{
		//TEST1_BITSET_AHIZEN_HIGH( &Si4709_dev.registers[TEST1] );
		//TEST1_BITSET_RESERVED( &Si4709_dev.registers[TEST1] );
 
		SYSCONFIG1_BITSET_GPIO_LOW(&Si4709_dev.registers[SYSCONFIG1]);
		SYSCONFIG1_BITSET_RESERVED( &Si4709_dev.registers[SYSCONFIG1] );
		/*VNVS: 13-OCT'09---- During Powerdown of the device RDS should be disabled 
							according to the Si4708/09 datasheet*/
		SYSCONFIG1_BITSET_RDS_LOW(&Si4709_dev.registers[SYSCONFIG1]);
 
		POWERCFG_BITSET_DMUTE_LOW( &Si4709_dev.registers[POWERCFG] );
		POWERCFG_BITSET_ENABLE_HIGH( &Si4709_dev.registers[POWERCFG] );
		POWERCFG_BITSET_DISABLE_HIGH( &Si4709_dev.registers[POWERCFG] );
		POWERCFG_BITSET_RESERVED( &Si4709_dev.registers[POWERCFG] );

		/*this will write all the above registers*/
		if( (ret = i2c_write( TEST1 )) < 0 )
		{
			debug("powerdown->i2c_write failed");
			Si4709_dev.registers[SYSCONFIG1] = sysconfig1;
			Si4709_dev.registers[POWERCFG] = powercfg;
			Si4709_dev.registers[TEST1] = test1;
		}
		else
		{
			Si4709_dev.state.power_state = RADIO_POWERDOWN;
		}

		 /****Resetting the device****/	
		gpio_set_value(FM_RESET, GPIO_LEVEL_LOW);
		gpio_set_value(FM_RESET, GPIO_LEVEL_HIGH);
	}
	else
		debug("Device already Powered-OFF");

    return ret;
}

static int seek(u32 *frequency, int up)
{
    int ret = 0;
    u16 powercfg = Si4709_dev.registers[POWERCFG];
    u16 channel = 0;
    int valid_station_found = 0;
 
    if( up ) 
    {
        POWERCFG_BITSET_SEEKUP_HIGH(&Si4709_dev.registers[POWERCFG]);
    }
    else
    {
        POWERCFG_BITSET_SEEKUP_LOW(&Si4709_dev.registers[POWERCFG]);
    }

    POWERCFG_BITSET_SKMODE_HIGH(&Si4709_dev.registers[POWERCFG]);
    POWERCFG_BITSET_SEEK_HIGH(&Si4709_dev.registers[POWERCFG]);
    POWERCFG_BITSET_RESERVED(&Si4709_dev.registers[POWERCFG]);

    if( (ret = i2c_write(POWERCFG)) < 0 )
    {	
        debug("seek i2c_write 1 failed");
        Si4709_dev.registers[POWERCFG] = powercfg;
    }
    else
    {
        Si4709_dev_wait_flag = SEEK_WAITING;

        wait();
		
		if ( Si4709_dev_wait_flag == SEEK_CANCEL ) 
		{
            powercfg = Si4709_dev.registers[POWERCFG];
            POWERCFG_BITSET_SEEK_LOW(&Si4709_dev.registers[POWERCFG]);
            POWERCFG_BITSET_RESERVED(&Si4709_dev.registers[POWERCFG]);
 
            if( (ret = i2c_write(POWERCFG)) < 0 )
            {
                debug("seek i2c_write 2 failed");
                Si4709_dev.registers[POWERCFG] = powercfg;
            }

			if( (ret = i2c_read( READCHAN )) < 0 )
			{
				debug("seek i2c_read 1 failed");
			}
			else
			{
				channel = READCHAN_GET_CHAN(Si4709_dev.registers[READCHAN]);
				*frequency = channel_to_freq(channel);
			}
			*frequency = 0;
		}

        Si4709_dev_wait_flag = NO_WAIT;
		            
        if( (ret = i2c_read(STATUSRSSI)) < 0 )
        {
            debug("seek i2c_read 2 failed");
        }
        else
        {
			/*VNVS:START 13-OCT'09---- Checking the status of Seek/Tune Bit*/
#ifdef TEST_FM
			if(STATUSRSSI_SEEK_TUNE_STATUS(Si4709_dev.registers[STATUSRSSI]) == COMPLETE)								
			{
				debug("Seek/Tune Status is set to 1 by device");
				if(STATUSRSSI_SF_BL_STATUS(Si4709_dev.registers[STATUSRSSI]) == SEEK_SUCCESSFUL)
				{
					debug("Seek Fail/Band Limit Status is set to 0 by device ---SeekUp Operation Completed");
					valid_station_found = 1;
				}
				else
					debug("Seek Fail/Band Limit Status is set to 1 by device ---SeekUp Operation Not Completed");
			}
			else
				debug("Seek/Tune Status is set to 0 by device ---SeekUp Operation Not Completed");		 			
			
#endif
			/*VNVS:END*/

            powercfg = Si4709_dev.registers[POWERCFG];
                    
            POWERCFG_BITSET_SEEK_LOW(&Si4709_dev.registers[POWERCFG]);
            POWERCFG_BITSET_RESERVED(&Si4709_dev.registers[POWERCFG]);
 
            if( (ret = i2c_write(POWERCFG)) < 0 )
            {
                debug("seek i2c_write 2 failed");
                Si4709_dev.registers[POWERCFG] = powercfg;
            }
            else
            {
                do
                {
                    if( (ret = i2c_read(STATUSRSSI)) < 0 )
                    {
                        debug("seek i2c_read 3 failed"); 
                        break;
                    }
                }while( STATUSRSSI_SEEK_TUNE_STATUS(Si4709_dev.registers[STATUSRSSI]) != CLEAR );

                if( ret == 0 && valid_station_found == 1 )
                {
                    if( (ret = i2c_read( READCHAN )) < 0 )
                    {
                        debug("seek i2c_read 4 failed");
                    }
                    else
                    {
                        channel = READCHAN_GET_CHAN(Si4709_dev.registers[READCHAN]);
                        *frequency = channel_to_freq(channel);
						debug("Frequency after seek-up is %d \n",*frequency);					
                    }
                }
                else
                {	
					debug("Valid station not found \n");										
					*frequency = 0;
				}
            }
        }    
    }

    return ret;
}

static int tune_freq(u32 frequency)
{
    int ret = 0;
    u16 channel = Si4709_dev.registers[CHANNEL];
#ifdef TEST_FM
	u16 read_channel;														
#endif
    debug("tune_freq called");

    Si4709_dev.registers[CHANNEL] = freq_to_channel(frequency);
#ifdef TEST_FM	
	read_channel = Si4709_dev.registers[CHANNEL];									
	debug(" Input read_channel =%x",read_channel);											
#endif
    CHANNEL_BITSET_TUNE_HIGH(&Si4709_dev.registers[CHANNEL]);
    CHANNEL_BITSET_RESERVED(&Si4709_dev.registers[CHANNEL]);

    if( (ret = i2c_write(CHANNEL)) < 0 )
    {
        debug("tune_freq i2c_write 1 failed");
        Si4709_dev.registers[CHANNEL] = channel;
    }
    else
    {	
			        
		Si4709_dev_wait_flag = TUNE_WAITING;
		debug("Si4709_dev_wait_flag = TUNE_WAITING");				
#ifdef TEST_FM	
		if( (ret = i2c_read(READCHAN)) < 0 )															
		{
			debug("tune_freq i2c_read 1 failed");
		}
		else
		{
			read_channel=READCHAN_GET_CHAN(Si4709_dev.registers[READCHAN]);											
			debug("curr_channel before tuning = %x",read_channel);														
		}																								
#endif																								

        wait();
		
		Si4709_dev_wait_flag = NO_WAIT;
		
		/*VNVS:START 13-OCT'09---- Checking the status of Seek/Tune Bit*/
#ifdef TEST_FM
		if( (ret = i2c_read(STATUSRSSI)) < 0 )																
        {
            debug("tune_freq i2c_read 2 failed"); 
           
        }
		else if(STATUSRSSI_SEEK_TUNE_STATUS(Si4709_dev.registers[STATUSRSSI]) == COMPLETE)							
			debug("Seek/Tune Status is set to 1 by device ---Tuning Operation Completed");
		else
			debug("Seek/Tune Status is set to 0 by device ---Tuning Operation Not Completed");	
#endif		
		/*VNVS:END*/

        channel = Si4709_dev.registers[CHANNEL];
    
        CHANNEL_BITSET_TUNE_LOW(&Si4709_dev.registers[CHANNEL]);
        CHANNEL_BITSET_RESERVED(&Si4709_dev.registers[CHANNEL]);
        
        if( (ret = i2c_write(CHANNEL)) < 0 )
        {
            debug("tune_freq i2c_write 2 failed");
            Si4709_dev.registers[CHANNEL] = channel;
        }
        else
        {
            do
            {
                if( (ret = i2c_read(STATUSRSSI)) < 0 )
                {
                     debug("tune_freq i2c_read 3 failed"); 
                     break;
                }
            }while( STATUSRSSI_SEEK_TUNE_STATUS(Si4709_dev.registers[STATUSRSSI]) != CLEAR );  
           
        }
		
		/*VNVS:START 13-OCT'09---- Reading the READCHAN register after tuning operation*/	
#ifdef TEST_FM
		if( (ret = i2c_read(READCHAN)) < 0 )																
		{
			debug("tune_freq i2c_read 2 failed");
		}
		else
		{
			read_channel=READCHAN_GET_CHAN(Si4709_dev.registers[READCHAN]);											
			debug("curr_channel after tuning= %x",read_channel);															
		}
#endif
		/*VNVS:END*/
    }

    return ret;
}

static void get_cur_chan_freq(u32 *frequency, u16 readchan)
{

    u16 channel = 0;
    debug("get_cur_chan_freq called"); 
	
	channel = READCHAN_GET_CHAN(readchan);
	debug("read_channel=%x",channel);											

    *frequency = channel_to_freq(channel);

    debug("frequency-> %u",*frequency);  
}

static u16 freq_to_channel(u32 frequency)
{
    u16 channel;

    if( frequency < Si4709_dev.settings.bottom_of_band )
    {
        frequency = Si4709_dev.settings.bottom_of_band;
    }

    channel = (frequency - Si4709_dev.settings.bottom_of_band) 
    	         / Si4709_dev.settings.channel_spacing;

    return channel;
}

static u32 channel_to_freq(u16 channel)
{
    u32 frequency;

    frequency = Si4709_dev.settings.bottom_of_band +
    	           Si4709_dev.settings.channel_spacing * channel;

    return frequency;
}

/*Only one thread will be able to call this, since this function call is 
   protected by a mutex, so no race conditions can arise*/
static void wait(void)
{
    wait_event_interruptible(Si4709_waitq, 
    	(Si4709_dev_wait_flag == WAIT_OVER) || (Si4709_dev_wait_flag == SEEK_CANCEL));
}

static void wait_RDS(void)
{
   wait_event_interruptible_timeout(Si4709_waitq, 
  	(Si4709_dev_wait_flag == WAIT_OVER),Si4709_dev.settings.timeout_RDS);
}

/*i2c read function*/
/*Si4709_dev.client should be set before calling this function.
   If Si4709_dev.valid = eTRUE then Si4709_dev.client will b valid
   This function should be called from the functions in this file. The 
   callers should check if Si4709_dev.valid = eTRUE before
   calling this function. If it is eFALSE then this function should not
   be called*/
static int i2c_read( u8 reg )
{
   u8 idx, reading_reg = STATUSRSSI;
  	u8 data[NUM_OF_REGISTERS * 2], data_high, data_low;
  	int msglen = 0, ret = 0;

  	for(idx = 0; idx < NUM_OF_REGISTERS * 2; idx++)
		{
		    data[idx] = 0x00;
  	} 

	  msglen = reg - reading_reg + 1;

	  if(msglen > 0)
	  {
		    msglen = msglen * 2;
	  } 
	  else
		{
		    msglen = (msglen + NUM_OF_REGISTERS) * 2;
	  } 

	  ret = i2c_master_recv((struct i2c_client*)(Si4709_dev.client), data, msglen);

	  if(ret == msglen) 
	  {
		    idx = 0;
		    do 
		    {
			       data_high	= data[idx];
			       data_low	= data[idx+1];

			       Si4709_dev.registers[reading_reg] = 0x0000;
			       Si4709_dev.registers[reading_reg] = (data_high << 8) + data_low;
			       reading_reg = (reading_reg + 1) & RDSD;
			       idx = idx + 2;
		     } while(reading_reg != ((reg +1) & RDSD));

        ret = 0;
	  }
	  else 
	  {
		      ret = -1;		
	  }

	  return ret;
}    

/*i2c write function*/
/*Si4709_dev.client should be set before calling this function.
   If Si4709_dev.valid = eTRUE then Si4709_dev.client will b valid
   This function should be called from the functions in this file. The 
   callers should check if Si4709_dev.valid = eTRUE before
   calling this function. If it is eFALSE then this function should not
   be called*/
static int i2c_write( u8 reg )
{
	   u8 writing_reg = POWERCFG;
	   u8 data[NUM_OF_REGISTERS * 2];
	   int i, msglen = 0, ret = 0;

	   for(i = 0; i < NUM_OF_REGISTERS * 2; i++)
	   	{
		      data[i] = 0x00;
	   	}
	   
	   do 
	   	{
		     data[msglen++] = (u8)(Si4709_dev.registers[writing_reg] >> 8);
		     data[msglen++] = (u8)(Si4709_dev.registers[writing_reg] & 0xFF);

	      	writing_reg = (writing_reg +1) & RDSD;
	    } while(writing_reg != ((reg + 1) & RDSD));

	    ret = i2c_master_send((struct i2c_client*)(Si4709_dev.client), ( const char *)data, msglen);

    	if(ret == msglen) 
    	{
		     ret = 0;
	    } 
    	else 
    	{
				  ret = -1;
	    }

    	return ret;
}    

static int  insert_preset(u32 frequency,u8 rssi,u8 *seek_preset_rssi)
{
    u8 i;
	  u8 min_rssi = 0xff;
   	u8 min_rssi_preset=0;
   	int ret = 0;
		   
	/* first find the minimum rssi and its location
	   this will always stop at the first location with a zero rssi */
	   
	   debug("si4709 autoseek : insert preset\n");
	
	   for (i=0; i<NUM_SEEK_PRESETS; i++) 
		 {
		     if (seek_preset_rssi[i] < min_rssi)
		     	 {
			         min_rssi = seek_preset_rssi[i];
			         min_rssi_preset = i;
		      }
	   }

	  if (rssi < min_rssi)
		 ret = -1;
	   
	/***Delete the preset with the minimum rssi, and clear the last preset
	       since it would only be a copy of the second to last preset after
	       the deletion ***/
	 for (i=min_rssi_preset; i<NUM_SEEK_PRESETS-1; i++)
		{
		     Si4709_dev.settings.seek_preset[i]= Si4709_dev.settings.seek_preset[i+1];
		     seek_preset_rssi[i] = seek_preset_rssi[i+1];
  	 }
	
	 Si4709_dev.settings.seek_preset[i] = 0;
	 seek_preset_rssi[i] = 0;

	/*** Fill the first preset with a zero for the frequency.  This will
	        always overwrite the last preset once all presets have been filled. ***/
	 for (i=min_rssi_preset; i<NUM_SEEK_PRESETS; i++)
		{
		     if(Si4709_dev.settings.seek_preset[i] == 0) 
		     	 {
			         Si4709_dev.settings.seek_preset[i]= frequency;
			         seek_preset_rssi[i] = rssi;
			         break;
		       }
	   }
	 return ret;
}	


