#include "sdkconfig.h"

#if CONFIG_LUA_RTOS_LUA_USE_APDS9960

#ifdef __cplusplus
extern "C"{
#endif

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/adds.h"
#include "freertos/timers.h"

#include "modules.h"
//#include "error.h"

#ifdef __cplusplus
  #include "lua.hpp"
#else
  #include "lua.h"
  #include "lualib.h"
  #include "lauxlib.h"
#endif

#include "apds9960.h"
#include <drivers/apds9960.h>

//static lua_Number u8_to_100 = 100.0/(2^8);
//static lua_Number u16_to_100 = 100.0/(2^16);

static uint8_t stdio;

static bool initialized = false;

TimerHandle_t apds9960_color_get_color_timer;
int apds9960_color_get_rgb_callback = LUA_REFNIL;
int apds9960_color_get_change_callback = LUA_REFNIL;

TimerHandle_t apds9960_proximity_get_thresh_timer;
int apds9960_proximity_get_thresh_callback = LUA_REFNIL;

int current_color_i = -1;
int prev_color_read = -1;
int saturation_threshold = 0;
int value_threshold = 0;
int n_colors = 0;
color_range *color_ranges;

int min_val = 20;
int max_val = 200;

int delta_h = 10;
int delta_s = 50;
int delta_v = 50;


int dist_threshold = 0;
int dist_histeresis = 0;
bool prev_state = false;

SparkFun_APDS9960 sensor;

static void RGB2HSV(struct RGB_set RGB, struct HSV_set &HSV);

static int find_color_in_range(int h, int s, int v) {
    if (v < min_val){
        return COLOR_BLACK_I; //black
    } else if (v > max_val){
        return COLOR_WHITE_I; //white
    } else {
        for (int color_i=0; color_i<n_colors; color_i++) {
            if (
                h>=color_ranges[color_i].h-delta_h && h<=color_ranges[color_i].h+delta_h &&
                s>=color_ranges[color_i].s-delta_s && s<=color_ranges[color_i].s+delta_s &&
                v>=color_ranges[color_i].v-delta_v && v<=color_ranges[color_i].v+delta_v
            ) {
                return color_i;
            }
        }
    }
    return COLOR_UNKNOWN_I; //unknown
}


static int apds9960_set_color_table(lua_State *L){

    if (lua_type(L,1)!=LUA_TTABLE) {
        lua_pushnil(L);
        lua_pushstring(L, "bad parameter, must be table");
    	return 2;
    }

    lua_len(L,1);
    n_colors = luaL_checkinteger( L, -1 );
    lua_pop(L,1);
    printf("Number of colors = %i\r\n", n_colors);

    color_ranges = new color_range [n_colors];

    for (int i=1; i<=n_colors; i++) {
        lua_rawgeti(L,1, i); // entry at stack top
        {
            if (lua_type(L,-1)!=LUA_TTABLE) {
                lua_pushnil(L);
                lua_pushstring(L, "bad entry in parameter, must be table");
            	return 2;
            }

            size_t str_len;
            lua_rawgeti(L,-1,1); // first entry

            if (lua_type(L,-1)!=LUA_TSTRING) {
                lua_pushnil(L);
                lua_pushstring(L, "bad entry in parameter, must be string");
            	return 2;
            }
            const char * colorName = lua_tolstring(L, -1, &str_len);
            color_ranges[i-1].name = new char[str_len+1];
            memcpy(color_ranges[i-1].name, colorName, str_len+1);
            lua_pop(L,1);


            lua_rawgeti(L,-1,2); // second entry
            color_ranges[i-1].h = lua_tointeger(L,-1);
            lua_pop(L,1);

            lua_rawgeti(L,-1,3); // third entry
            color_ranges[i-1].s = lua_tointeger(L,-1);
            lua_pop(L,1);

            lua_rawgeti(L,-1,4); // fourth entry
            color_ranges[i-1].v = lua_tointeger(L,-1);
            lua_pop(L,1);
            
            printf("Added -> color = %s, (h,s,v) = (%i,%i,%i)\r\n", color_ranges[i-1].name, color_ranges[i-1].h, color_ranges[i-1].s,color_ranges[i-1].v);

        }
        lua_pop(L, 1); // pop entry
    }
    lua_pushboolean(L, true);
    return 1;
}

static int apds9960_set_sv_limits (lua_State *L) {
    delta_h = luaL_checkinteger( L, 1 );
    delta_s = luaL_checkinteger( L, 2 );
    delta_v = luaL_checkinteger( L, 3 );
    min_val = luaL_optinteger( L, 4, min_val );
    max_val = luaL_optinteger( L, 5, max_val );
    
    printf("Color limits set dh=%i ds=%i dv=%i minv=%i maxv=%i\r\n",delta_h, delta_s, delta_v, min_val, max_val);
       
    lua_pushboolean(L, true);
    return 1;
}

/*
static int apds9960_rgb2hsvcolor (lua_State *L) {
    HSV_set hsv;
    RGB_set rgb;
    rgb.r = luaL_checkinteger( L, 1 ); //R<<8;
    rgb.g = luaL_checkinteger( L, 2 ); //G<<8;
    rgb.b = luaL_checkinteger( L, 3 ); //B<<8;
    RGB2HSV(rgb , hsv);
    
    lua_pushinteger(L, hsv.h);
    lua_pushinteger(L, hsv.s);
    lua_pushinteger(L, hsv.v);
    
    int color_i = find_color_in_range(hsv.h, hsv.s, hsv.v);
    if (color_i >= 0){
      lua_pushstring(L, color_ranges[color_i].name);
    }else{
      if (color_i == -3){
        lua_pushstring(L, COLOR_WHITE);
      }else if (color_i == -2){
        lua_pushstring(L, COLOR_BLACK);
      }else{
        lua_pushstring(L, COLOR_UNKNOWN);
      }
    }

    return 4;
}
*/

static void callback_sw_get_color(TimerHandle_t xTimer) {
	lua_State *TL;
	lua_State *L;
	int tref;

    // Set standards streams
    if (!stdio) {
        __getreent()->_stdin  = _GLOBAL_REENT->_stdin;
        __getreent()->_stdout = _GLOBAL_REENT->_stdout;
        __getreent()->_stderr = _GLOBAL_REENT->_stderr;

        // Work-around newlib is not compiled with HAVE_BLKSIZE flag
        setvbuf(_GLOBAL_REENT->_stdin , NULL, _IONBF, 0);
        setvbuf(_GLOBAL_REENT->_stdout, NULL, _IONBF, 0);
        setvbuf(_GLOBAL_REENT->_stderr, NULL, _IONBF, 0);

        stdio = 1;
    }


    uint16_t R;
    uint16_t G;
    uint16_t B;
    uint16_t A;
    //printf ("to read\r\n");
    bool ok = sensor.readColors(R, G, B, A);
    //printf ("after read\r\n");

    int status;
    if (ok) {
        
        HSV_set hsv;
        RGB_set rgb;
        rgb.r = R; //R<<8;
        rgb.g = G; //G<<8;
        rgb.b = B; //B<<8;
        RGB2HSV(rgb, hsv);
        
        
        
        ////////////
        if (apds9960_color_get_rgb_callback!=LUA_REFNIL) {
            L = pvGetLuaState();
            TL = lua_newthread(L);

            tref = luaL_ref(L, LUA_REGISTRYINDEX);

            lua_rawgeti(L, LUA_REGISTRYINDEX, apds9960_color_get_rgb_callback);
            lua_xmove(L, TL, 1);
               
            /*
            lua_pushnumber(TL, (lua_Number)R*u16_to_100);
            lua_pushnumber(TL, (lua_Number)G*u16_to_100);
            lua_pushnumber(TL, (lua_Number)B*u16_to_100);
            lua_pushnumber(TL, (lua_Number)A*u16_to_100);
            lua_pushnumber(TL, hsv.h);
            lua_pushnumber(TL, (lua_Number)hsv.s*u8_to_100);
            lua_pushnumber(TL, (lua_Number)hsv.v*u8_to_100);
            */
            
            lua_pushinteger(TL, R);
            lua_pushinteger(TL, G);
            lua_pushinteger(TL, B);
            lua_pushinteger(TL, A);
            lua_pushinteger(TL, hsv.h);
            lua_pushinteger(TL, hsv.s);
            lua_pushinteger(TL, hsv.v);
            status = lua_pcall(TL, 7, 0, 0);
            luaL_unref(TL, LUA_REGISTRYINDEX, tref);
            if (status != LUA_OK) {
		        const char *msg = lua_tostring(TL, -1);
		        lua_writestringerror("error in color_rgb callback: %s\n", msg);
		        lua_pop(TL, 1);		
                //luaL_error(TL, msg);
            }   
        }       
        ////////////
        

        /*
        if (hsv.s<saturation_threshold || hsv.v<value_threshold) {
            return;
        }
        */

        int color_i = find_color_in_range(hsv.h, hsv.s, hsv.v);
        //printf("----current_color_i %i    color_i %i    prev_color_read %i\r\n", current_color_i, color_i, prev_color_read);

        if (apds9960_color_get_change_callback!=LUA_REFNIL && color_i!=current_color_i && prev_color_read == color_i) {
            current_color_i = color_i;

            //prepare thread
            L = pvGetLuaState();
            TL = lua_newthread(L);
            tref = luaL_ref(L, LUA_REGISTRYINDEX);
            lua_rawgeti(L, LUA_REGISTRYINDEX, apds9960_color_get_change_callback);
            lua_xmove(L, TL, 1);

            if (color_i >= 0){
              lua_pushstring(TL, color_ranges[color_i].name);
            }else{
              if (color_i == COLOR_WHITE_I){
                lua_pushstring(TL, COLOR_WHITE);
              }else if (color_i == COLOR_BLACK_I){
                lua_pushstring(TL, COLOR_BLACK);
              }else{
                lua_pushstring(TL, COLOR_UNKNOWN);
              }
            }
            
            /*
            lua_pushinteger(TL, hsv.h);
            lua_pushnumber(TL, (lua_Number)hsv.s*u8_to_100);
            lua_pushnumber(TL, (lua_Number)hsv.v*u8_to_100);
            */

            lua_pushinteger(TL, hsv.h);
            lua_pushinteger(TL, hsv.s);
            lua_pushinteger(TL, hsv.v);
            status = lua_pcall(TL, 4, 0, 0);
            luaL_unref(TL, LUA_REGISTRYINDEX, tref);

            if (status != LUA_OK) {
            
		        const char *msg = lua_tostring(TL, -1);
		        lua_writestringerror("error in color_change callback: %s\n", msg);
		        lua_pop(TL, 1);		
                //luaL_error(TL, msg);
            
            }
            prev_color_read = color_i;
        } else {
            prev_color_read = color_i;
            return; //no changes
        }
    } else {
printf("Error in sensor.readColor: %i", ok);
/*
        //prepare thread
        L = pvGetLuaState();
        TL = lua_newthread(L);
        tref = luaL_ref(L, LUA_REGISTRYINDEX);
        lua_rawgeti(L, LUA_REGISTRYINDEX, apds9960_color_get_change_callback);
        lua_xmove(L, TL, 1);

        lua_pushnil(TL);
        lua_pushstring(TL, "failure");
        status = lua_pcall(TL, 2, 0, 0);
        luaL_unref(TL, LUA_REGISTRYINDEX, tref);

        if (status != LUA_OK) {
		    const char *msg = lua_tostring(TL, -1);
		    lua_writestringerror("error in color_rgb callback %s\n", msg);
		    lua_pop(TL, 1);		
            //luaL_error(TL, msg);
        }
  */
    }

    
}

static int apds9960_init (lua_State *L) {
    if (!initialized) {
        bool ok = sensor.init();
        if (!ok) {
            lua_pushnil(L);
            lua_pushstring(L, "init failed");
            return 2;
        }
        ok = sensor.enablePower();
        if (!ok) {
            lua_pushnil(L);
            lua_pushstring(L, "failure enabling power");
            return 2;
        }
        
    }
    initialized = true;
    lua_pushboolean(L, true);
    return 1;
}

static int apds9960_enable_power (lua_State *L) {
    bool enable = lua_gettop(L)==0 || lua_toboolean( L, 1 );
    bool ok;
    if (enable) {
        ok = sensor.enablePower();
    } else {
        ok = sensor.disablePower();
    }
    if (!ok) {
        lua_pushnil(L);
        lua_pushstring(L, "failure");
        lua_pushboolean(L, enable);
        return 3;
    }
    lua_pushboolean(L, true);
    return 1;
}


static int apds9960_color_set_ambient_gain (lua_State *L) {
    bool gain = lua_tointeger( L, 1 );
    bool ok;
    ok = sensor.setAmbientLightGain(gain);
    if (!ok) {
        lua_pushnil(L);
        lua_pushstring(L, "failure");
        lua_pushinteger(L, gain);
        return 3;
    }
    lua_pushboolean(L, true);
    return 1;
}

static int apds9960_set_LED_drive (lua_State *L) {
    bool drive = lua_tointeger( L, 1 );
    bool ok;
    ok = sensor.setLEDDrive(drive);
    if (!ok) {
        lua_pushnil(L);
        lua_pushstring(L, "failure");
        lua_pushinteger(L, drive);
        return 3;
    }
    lua_pushboolean(L, true);
    return 1;
}


static void callback_dist_get_dist_thresh(TimerHandle_t xTimer) {
	lua_State *TL;
	lua_State *L;
	int tref;

    // Set standards streams
    if (!stdio) {
        __getreent()->_stdin  = _GLOBAL_REENT->_stdin;
        __getreent()->_stdout = _GLOBAL_REENT->_stdout;
        __getreent()->_stderr = _GLOBAL_REENT->_stderr;

        // Work-around newlib is not compiled with HAVE_BLKSIZE flag
        setvbuf(_GLOBAL_REENT->_stdin , NULL, _IONBF, 0);
        setvbuf(_GLOBAL_REENT->_stdout, NULL, _IONBF, 0);
        setvbuf(_GLOBAL_REENT->_stderr, NULL, _IONBF, 0);

        stdio = 1;
    }


    uint8_t d;
    bool ok = sensor.readProximity(d);

    int status;
    if (ok) {
        if ((prev_state && (d < dist_threshold)) || (!prev_state && (d > dist_threshold + dist_histeresis))){
           prev_state = !prev_state;

           L = pvGetLuaState();
           TL = lua_newthread(L);
           tref = luaL_ref(L, LUA_REGISTRYINDEX);
           lua_rawgeti(L, LUA_REGISTRYINDEX, apds9960_proximity_get_thresh_callback);
           lua_xmove(L, TL, 1);

           lua_pushboolean(TL, prev_state);

           status = lua_pcall(TL, 1, 0, 0);
           luaL_unref(TL, LUA_REGISTRYINDEX, tref);

        } else {
            return; //no changes
        }

    } else {
        //prepare thread
        L = pvGetLuaState();
        TL = lua_newthread(L);
        tref = luaL_ref(L, LUA_REGISTRYINDEX);
        lua_rawgeti(L, LUA_REGISTRYINDEX, apds9960_proximity_get_thresh_callback);
        lua_xmove(L, TL, 1);

        lua_pushnil(TL);
        lua_pushstring(TL, "failure");
        status = lua_pcall(TL, 2, 0, 0);
        luaL_unref(TL, LUA_REGISTRYINDEX, tref);
    }

    if (status != LUA_OK) {
		const char *msg = lua_tostring(TL, -1);
    	//luaL_error(TL, msg);
		lua_writestringerror("error in proximity callback %s\n", msg);
		lua_pop(TL, 1);		
    }
}

static int apds9960_color_get_change (lua_State *L) {
    bool enable = lua_toboolean(L, 1);
    if (enable) {
	    luaL_checktype(L, 1, LUA_TFUNCTION);
        lua_pushvalue(L, 1);
        apds9960_color_get_change_callback = luaL_ref(L, LUA_REGISTRYINDEX);
    } else {
        if (apds9960_color_get_change_callback==LUA_REFNIL) {
            lua_pushnil(L);
            lua_pushstring(L, "no continuos get running");
            return 2;
        }
        apds9960_color_get_change_callback = LUA_REFNIL;
    }

    lua_pushboolean(L, true);
	return 1;
}

static int apds9960_color_get_continuous (lua_State *L) {
    bool enable = lua_toboolean(L, 1);
    if (enable) {
	    luaL_checktype(L, 1, LUA_TFUNCTION);
        lua_pushvalue(L, 1);
        apds9960_color_get_rgb_callback = luaL_ref(L, LUA_REGISTRYINDEX);
    } else {
        if (apds9960_color_get_rgb_callback==LUA_REFNIL) {
            lua_pushnil(L);
            lua_pushstring(L, "no continuos get running");
            return 2;
        }
        apds9960_color_get_rgb_callback = LUA_REFNIL;
    }

    lua_pushboolean(L, true);
	return 1;
}


static int apds9960_proximity_get_thresh (lua_State *L) {
    bool enable = lua_toboolean(L, 1);
    if (enable) {
        luaL_checktype(L, 1, LUA_TFUNCTION);
        lua_pushvalue(L, 1);
        apds9960_proximity_get_thresh_callback = luaL_ref(L, LUA_REGISTRYINDEX);
    } else {
        if (apds9960_proximity_get_thresh_callback==LUA_REFNIL) {
            lua_pushnil(L);
            lua_pushstring(L, "no proximity get running");
            return 2;
        }
        apds9960_proximity_get_thresh_callback = LUA_REFNIL;
    }

    lua_pushboolean(L, true);
	return 1;
}


static int apds9960_proximity_enable (lua_State *L) {
    bool enable = lua_toboolean(L, 1);
    if (enable) {
        uint32_t millis = luaL_checkinteger( L, 1 );
	    if (millis < 1) {
            lua_pushnil(L);
            lua_pushstring(L, "invalid period");
            return 2;
	    }
	    
        dist_threshold = luaL_checkinteger( L, 2 );
        if (dist_threshold < 0) {
            lua_pushnil(L);
            lua_pushstring(L, "invalid thresh");
            return 2;
        }

        dist_histeresis = luaL_checkinteger( L, 3 );
        if (dist_histeresis < 0) {
            lua_pushnil(L);
            lua_pushstring(L, "invalid histeresis");
            return 2;
        }
	    
	    if (!sensor.enableProximitySensor(false)) {
            lua_pushnil(L);
            lua_pushstring(L, "failure to enable sensor");
            return 2;
	    }
	    
        //set timer for callback
        apds9960_proximity_get_thresh_timer = xTimerCreate("apds_prox", millis / portTICK_PERIOD_MS, pdTRUE,
            (void *)apds9960_proximity_get_thresh_timer, callback_dist_get_dist_thresh);
        xTimerStart(apds9960_proximity_get_thresh_timer, 0);
    } else {

	    if (!sensor.disableProximitySensor()) {
            lua_pushnil(L);
            lua_pushstring(L, "failure to disable sensor");
            return 2;
	    }

        //delete timer
        xTimerStop(apds9960_proximity_get_thresh_timer, portMAX_DELAY);
	      xTimerDelete(apds9960_proximity_get_thresh_timer, portMAX_DELAY);
        apds9960_proximity_get_thresh_callback = LUA_REFNIL;
    }

    lua_pushboolean(L, true);
	return 1;
}


static int apds9960_color_enable (lua_State *L) {
    bool enable = lua_toboolean(L, 1);
    if (enable) {
        uint32_t millis = luaL_checkinteger( L, 1 );
	    if (millis < 1) {
            lua_pushnil(L);
            lua_pushstring(L, "invalid period");
            return 2;
	    }
	    
        if (!sensor.enableLightSensor(false) ) {
            lua_pushnil(L);
            lua_pushstring(L, "failure to enable sensor");
            return 2;
        }

        //set timer for callback
        apds9960_color_get_color_timer = xTimerCreate("apds_color", millis / portTICK_PERIOD_MS, pdTRUE,
            (void *)apds9960_color_get_color_timer, callback_sw_get_color);
        xTimerStart(apds9960_color_get_color_timer, 0);
    } else {

        if (!sensor.disableLightSensor() ) {
            lua_pushnil(L);
            lua_pushstring(L, "failure to disable sensor");
            return 2;
        }

        //delete timer
        xTimerStop(apds9960_color_get_color_timer, portMAX_DELAY);
		xTimerDelete(apds9960_color_get_color_timer, portMAX_DELAY);
    }

    lua_pushboolean(L, true);
	return 1;
}


/*******************************************************************************
 * Function RGB2HSV
 * Description: Converts an RGB color value into its equivalen in the HSV color space.
 * Copyright 2010 by George Ruinelli
 * The code I used as a source is from http://www.cs.rit.edu/~ncs/color/t_convert.html
 * Parameters:
 *   1. struct with RGB color (source)
 *   2. pointer to struct HSV color (target)
 * Notes:
 *   - r, g, b values are from 0..255
 *   - h = [0,360], s = [0,255], v = [0,255]
 *   - NB: if s == 0, then h = 0 (undefined)
 ******************************************************************************/
static void RGB2HSV(struct RGB_set RGB, struct HSV_set &HSV){
    uint16_t min, max, delta;

    if(RGB.r<RGB.g)min=RGB.r; else min=RGB.g;
    if(RGB.b<min)min=RGB.b;

    if(RGB.r>RGB.g)max=RGB.r; else max=RGB.g;
    if(RGB.b>max)max=RGB.b;

    HSV.v = max;                // v, 16bit

    delta = max - min;          // 16bit, < v

    if( max != 0 )
        HSV.s = (int)(delta)*2^16 / max;        // s, 16bit
    else {
        // r = g = b = 0        // s = 0, v is undefined
        HSV.s = 0;
        HSV.h = 0;
        return;
    }

    if( delta == 0 )
        HSV.h = 0;
    else if( RGB.r == max )
        HSV.h = (RGB.g - RGB.b)*60/delta;        // between yellow & magenta
    else if( RGB.g == max )
        HSV.h = 120 + (RGB.b - RGB.r)*60/delta;    // between cyan & yellow
    else
        HSV.h = 240 + (RGB.r - RGB.g)*60/delta;    // between magenta & cyan

    if( HSV.h < 0 )
        HSV.h += 360;
}


static const luaL_Reg apds9960[] = {
    {"init", apds9960_init},
	{"enable_power", apds9960_enable_power},
	{"set_LED_drive", apds9960_set_LED_drive},
    {NULL, NULL}
};


static const luaL_Reg apds9960_color[] = {
    {"enable", apds9960_color_enable},
    {"set_rgb_callback", apds9960_color_get_continuous},
    {"set_color_callback", apds9960_color_get_change},
    {"set_ambient_gain", apds9960_color_set_ambient_gain},
    {"set_color_table", apds9960_set_color_table},
    {"set_sv_limits", apds9960_set_sv_limits},
    //{"rgb2hsvcolor", apds9960_rgb2hsvcolor},
    {NULL, NULL}
};

static const luaL_Reg apds9960_proximity[] = {
    {"enable", apds9960_proximity_enable},
    {"set_callback", apds9960_proximity_get_thresh},
    {NULL, NULL}
};


LUALIB_API int luaopen_apds9960( lua_State *L ) {

    //register
    luaL_newlib(L, apds9960);

    lua_newtable(L);
    luaL_setfuncs((L), apds9960_color, 0);
    lua_setfield(L, -2, "color");

    lua_newtable(L);
    luaL_setfuncs((L), apds9960_proximity, 0);
    lua_setfield(L, -2, "proximity");

	return 1;
}

MODULE_REGISTER_RAM(APDS9960, apds9960, luaopen_apds9960, 1);

#ifdef __cplusplus
}
#endif

#endif
