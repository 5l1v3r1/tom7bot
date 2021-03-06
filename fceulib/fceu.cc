/* FCE Ultra - NES/Famicom Emulator
*
* Copyright notice for this file:
*  Copyright (C) 2003 Xodnizel
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include <string>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include "types.h"
#include "x6502.h"
#include "fceu.h"
#include "ppu.h"
#include "sound.h"
#include "file.h"
#include "utils/endian.h"
#include "utils/memory.h"
#include "utils/crc32.h"

#include "cart.h"
#include "fds.h"
#include "ines.h"
#include "unif.h"
#include "palette.h"
#include "state.h"
#include "input.h"
#include "file.h"
#include "vsuni.h"
#include "ines.h"

#include "driver.h"

#include "tracing.h"

#include <fstream>
#include <sstream>

using namespace std;

// Used by some boards to do delayed memory writes, etc.
uint64 timestampbase = 0ULL;
FCEUGI *GameInfo = nullptr;

void (*GameInterface)(GI h);
void (*GameStateRestore)(int version);

readfunc ARead[0x10000];
writefunc BWrite[0x10000];
static readfunc *AReadG;
static writefunc *BWriteG;
static constexpr int RWWrap = 0;
uint8 *GameMemBlock = nullptr;
uint8 *RAM = nullptr;
uint8 *XBuf = nullptr;
uint8 *XBackBuf = nullptr;

FCEUGI::FCEUGI() { }

FCEUGI::~FCEUGI() { }

void FCEU_CloseGame() {
  if (GameInfo) {
    GameInterface(GI_CLOSE);

    ResetExState(0,0);

    //clear screen when game is closed
    extern uint8 *XBuf;
    if (XBuf)
      memset(XBuf,0,256*256);

    delete GameInfo;
    GameInfo = nullptr;
  }
}

static DECLFW(BNull) {
}

static DECLFR(ANull) {
  TRACEF("Read unmapped: %02x", X.DB);
  return X.DB;
}

readfunc GetReadHandler(int32 a) {
  if (a>=0x8000 && RWWrap)
    return AReadG[a-0x8000];
  else
    return ARead[a];
}

writefunc GetWriteHandler(int32 a) {
  if (RWWrap && a >= 0x8000)
    return BWriteG[a - 0x8000];
  else
    return BWrite[a];
}

void SetWriteHandler(int32 start, int32 end, writefunc func) {
  if (!func)
    func = BNull;

  if (RWWrap) {
    for (int32 x = end; x >= start; x--) {
      if (x>=0x8000)
        BWriteG[x-0x8000]=func;
      else
        BWrite[x]=func;
    } 
  } else {
    for (int32 x = end; x >= start; x--) {
      BWrite[x]=func;
    }
  }
}

static void AllocBuffers() {
  GameMemBlock = (uint8*)FCEU_gmalloc(GAME_MEM_BLOCK_SIZE);
  RAM = (uint8*)FCEU_gmalloc(0x800);
  XBuf = (uint8*)FCEU_gmalloc(256 * 256);
  XBackBuf = (uint8*)FCEU_gmalloc(256 * 256);
}

static void FreeBuffers() {
  free(GameMemBlock);
  free(RAM);
  free(XBuf);
  free(XBackBuf);
}

// TODO tom7: Merge this with fsettings_pal.
uint8 PAL = 0;
int fsettings_pal = 0;

static DECLFW(BRAML) {
  RAM[A]=V;
}

static DECLFW(BRAMH) {
  RAM[A&0x7FF]=V;
}

static DECLFR(ARAML) {
  return RAM[A];
}

static DECLFR(ARAMH) {
  return RAM[A&0x7FF];
}

static void ResetGameLoaded() {
  if (GameInfo) FCEU_CloseGame();
  GameStateRestore=0;
  fceulib__ppu.PPU_hook=0;
  fceulib__ppu.GameHBIRQHook=0;
  // Probably this should happen within sound itself.
  if (fceulib__sound.GameExpSound.Kill)
    fceulib__sound.GameExpSound.Kill();
  memset(&fceulib__sound.GameExpSound, 0,
         sizeof (fceulib__sound.GameExpSound));

  X.MapIRQHook = nullptr;
  fceulib__ppu.MMC5Hack = 0;
  PAL &= 1;
  fceulib__palette.pale = 0;
}

FCEUGI *FCEUI_LoadGameVirtual(const char *name, int OverwriteVidMode) {
  //----------
  //attempt to open the files
  FCEU_printf("Loading %s...\n\n",name);

  FceuFile *fp = FCEU_fopen(name,"rb",0);

  if (!fp) {
    FCEU_PrintError("Error opening \"%s\"!",name);
    return 0;
  }

  // file opened ok. start loading.

  ResetGameLoaded();

  // reset parameters so they're cleared just in case a format's loader
  // doesnt know to do the clearing
  fceulib__ines.ClearMasterRomInfoParams();

  FCEU_CloseGame();
  GameInfo = new FCEUGI();
  memset(GameInfo, 0, sizeof(FCEUGI));

  GameInfo->soundchan = 0;
  GameInfo->soundrate = 0;
  GameInfo->type = GIT_CART;
  GameInfo->vidsys = GIV_USER;
  GameInfo->input[0] = GameInfo->input[1] = SI_UNSET;
  GameInfo->inputfc = SIFC_UNSET;
  GameInfo->cspecial = SIS_NONE;

  // Try to load each different format
  if (fceulib__ines.iNESLoad(name, fp, OverwriteVidMode))
    goto endlseq;
  if (fceulib__unif.UNIFLoad(name, fp))
    goto endlseq;
  if (fceulib__fds.FDSLoad(name, fp))
    goto endlseq;

  FCEU_PrintError("An error occurred while loading the file.");
  FCEU_fclose(fp);

  delete GameInfo;
  GameInfo = 0;

  return 0;

endlseq:

  FCEU_fclose(fp);

  FCEU_ResetVidSys();


  PowerNES();
  TRACEF("PowerNES done.");
  // TRACEA(WRAM, sizeofWRAM);

  fceulib__palette.LoadGamePalette();
  fceulib__palette.ResetPalette();

  return GameInfo;
}

FCEUGI *FCEUI_LoadGame(const char *name, int OverwriteVidMode) {
  return FCEUI_LoadGameVirtual(name, OverwriteVidMode);
}


// Return: Flag that indicates whether the function was succesful or not.
bool FCEUI_Initialize() {
  // XXX I think we shouldn't do anything randomly in fceulib --tom7.
  srand(time(0));

  AllocBuffers();

  X.Init();

  return true;
}

void FCEUI_Kill() {
  FreeBuffers();
}

// Emulates a single frame.
// Skip may be passed in, if FRAMESKIP is #defined, to cause this to
// emulate more than one frame
// skip initiates frame skip if 1, or frame skip and sound skip if 2
void FCEUI_Emulate(uint8 **pXBuf, int32 **SoundBuf, int32 *SoundBufSize, 
                   int skip) {
  fceulib__input.FCEU_UpdateInput();

  // fprintf(stderr, "ppu loop..\n");

  (void)fceulib__ppu.FCEUPPU_Loop(skip);

  // fprintf(stderr, "sound thing loop skip=%d..\n", skip);

  int ssize = 0;
  // If skip = 2 we are skipping sound processing
  if (skip != 2)
    ssize = fceulib__sound.FlushEmulateSound();

  // This is where cheat list stuff happened.
  timestampbase += X.timestamp;
  X.timestamp = 0;

  if (pXBuf != nullptr) {
    *pXBuf=skip?0:XBuf;
  }

  // If skip = 2, then bypass sound.
  if (skip == 2) {
    *SoundBuf=0;
    *SoundBufSize=0;
  } else {
    *SoundBuf=fceulib__sound.WaveFinal;
    *SoundBufSize=ssize;
  }
}

void ResetNES() {
  if (!GameInfo) return;
  GameInterface(GI_RESETM2);
  fceulib__sound.FCEUSND_Reset();
  fceulib__ppu.FCEUPPU_Reset();
  X.Reset();

  // clear back baffer
  extern uint8 *XBackBuf;
  memset(XBackBuf,0,256*256);

  fprintf(stderr, "Reset\n");
}

void FCEU_MemoryRand(uint8 *ptr, uint32 size) {
  int x=0;
  while(size) {
    *ptr = (x&4) ? 0xFF : 0x00;
    x++;
    size--;
    ptr++;
  }
}

void PowerNES() {
  if (!GameInfo) return;

  FCEU_MemoryRand(RAM,0x800);

  SetReadHandler(0x0000,0xFFFF,ANull);
  SetWriteHandler(0x0000,0xFFFF,BNull);

  SetReadHandler(0,0x7FF,ARAML);
  SetWriteHandler(0,0x7FF,BRAML);

  SetReadHandler(0x800,0x1FFF,ARAMH); // Part of a little
  SetWriteHandler(0x800,0x1FFF,BRAMH); //hack for a small speed boost.

  fceulib__input.InitializeInput();
  fceulib__sound.FCEUSND_Power();
  fceulib__ppu.FCEUPPU_Power();

  // Have the external game hardware "powered" after the internal NES
  // stuff. Needed for the NSF code and VS System code.
  GameInterface(GI_POWER);
  if (GameInfo->type==GIT_VSUNI)
    fceulib__vsuni.FCEU_VSUniPower();

  // if we are in a movie, then reset the saveram
  if (fceulib__cart.disableBatteryLoading)
    GameInterface(GI_RESETSAVE);

  timestampbase = 0ULL;
  X.Power();

  // clear back baffer
  extern uint8 *XBackBuf;
  memset(XBackBuf,0,256*256);

  fprintf(stderr, "Power on\n");
}

void FCEU_ResetVidSys() {
  int w;

  if (GameInfo->vidsys==GIV_NTSC)
    w = 0;
  else if (GameInfo->vidsys==GIV_PAL)
    w = 1;
  else
    w = fsettings_pal;

  PAL = !!w;
  fceulib__ppu.FCEUPPU_SetVideoSystem(w);
  fceulib__sound.SetSoundVariables();
}

// This is kind of silly since it just eta-expands printf with
// a limit of 2k. Probably nothing should be printing in fceulib
// unless there's an error, though.
void FCEU_printf(char *format, ...) {
  char temp[2048];

  va_list ap;

  va_start(ap,format);
  vsnprintf(temp,sizeof(temp),format,ap);

  fputs(temp, stdout);

  va_end(ap);
}

void FCEU_PrintError(char *format, ...) {
  char temp[2048];

  va_list ap;

  va_start(ap,format);
  vsnprintf(temp,sizeof(temp),format,ap);
  FCEUD_PrintError(temp);

  va_end(ap);
}

void FCEUI_SetVidSystem(int a) {
  fsettings_pal = a ? 1 : 0;
  if (GameInfo) {
    FCEU_ResetVidSys();
    fceulib__palette.ResetPalette();
  }
}

bool FCEU_IsValidUI(EFCEUI ui) {
  switch(ui) {
  case FCEUI_RESET:
  case FCEUI_POWER:
  case FCEUI_EJECT_DISK:
  case FCEUI_SWITCH_DISK:
    if (!GameInfo) return false;
    break;
  }
  return true;
}

void SetReadHandler(int32 start, int32 end, readfunc func) {
  if (!func)
    func = ANull;

  if (RWWrap) {
    for (int32 x = end; x >= start; x--) {
      if (x >= 0x8000)
        AReadG[x - 0x8000] = func;
      else
        ARead[x]=func;
    }
  } else {
    for (int x = end; x >= start; x--) {
      ARead[x] = func;
    }
  }
}
