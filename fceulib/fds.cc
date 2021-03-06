/* FCE Ultra - NES/Famicom Emulator
*
* Copyright notice for this file:
*  Copyright (C) 2002 Xodnizel
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "types.h"
#include "x6502.h"
#include "fceu.h"
#include "fds.h"
#include "sound.h"
#include "file.h"
#include "utils/md5.h"
#include "utils/memory.h"
#include "state.h"
#include "file.h"
#include "cart.h"
#include "driver.h"
#include "fsettings.h"

#include "tracing.h"

//  TODO:  Add code to put a delay in between the time a disk is inserted
//	and the when it can be successfully read/written to.  This should
//	prevent writes to wrong places OR add code to prevent disk ejects
//	when the virtual motor is on.

static DECLFR(FDSRead4030);
static DECLFR(FDSRead4031);
static DECLFR(FDSRead4032);
static DECLFR(FDSRead4033);

static DECLFW(FDSWrite);

static DECLFW(FDSWaveWrite);
static DECLFR(FDSWaveRead);

static DECLFR(FDSSRead);
static DECLFW(FDSSWrite);
static DECLFR(FDSBIOSRead);
static DECLFR(FDSRAMRead);
static DECLFW(FDSRAMWrite);

#define FDSRAM GameMemBlock
#define CHRRAM (GameMemBlock+32768)

static void FDSGI_Trampoline(GI h) {
  return fceulib__fds.FDSGI(h);
}

void FDS::FDSGI(GI h) {
  switch(h) {
  case GI_CLOSE: FDSClose(); break;
  case GI_POWER: FDSInit(); break;
  default:;
  }
}

void FDS::FDSStateRestore(int version) {
  fceulib__cart.setmirror(((FDSRegs[5]&8)>>3)^1);

  if (version >= 9810) {
    for (int x=0;x<TotalSides;x++) {
      for (int b=0; b<65500; b++) {
	diskdata[x][b] ^= diskdatao[x][b];
      }
    }
  }
}

static void FDSFix_Trampoline(int a) {
  return fceulib__fds.FDSFix(a);
}

static void FDSStateRestore_Trampoline(int version) {
  return fceulib__fds.FDSStateRestore(version);
}

void FDS::FDSInit() {
  memset(FDSRegs,0,sizeof(FDSRegs));
  writeskip=DiskPtr=DiskSeekIRQ=0;
  fceulib__cart.setmirror(1);

  fceulib__cart.setprg8r(0,0xe000,0);    // BIOS
  fceulib__cart.setprg32r(1,0x6000,0);   // 32KB RAM
  fceulib__cart.setchr8(0);     // 8KB CHR RAM

  X.MapIRQHook = FDSFix_Trampoline;
  GameStateRestore=FDSStateRestore_Trampoline;

  SetReadHandler(0x4030,0x4030,FDSRead4030);
  SetReadHandler(0x4031,0x4031,FDSRead4031);
  SetReadHandler(0x4032,0x4032,FDSRead4032);
  SetReadHandler(0x4033,0x4033,FDSRead4033);

  SetWriteHandler(0x4020,0x4025,FDSWrite);

  SetWriteHandler(0x6000,0xdfff,FDSRAMWrite);
  SetReadHandler(0x6000,0xdfff,FDSRAMRead);
  SetReadHandler(0xE000,0xFFFF,FDSBIOSRead);
  IRQCount=IRQLatch=IRQa=0;

  FDSSoundReset();
  InDisk=0;
  SelectDisk=0;
}

void FDS::FCEU_FDSInsert() {
  if (TotalSides==0) {
    fprintf(stderr, "Not FDS; can't eject disk.");
    return;
  }
  if (InDisk==255) {
    fprintf(stderr, "Disk %d Side %s Inserted",SelectDisk>>1,(SelectDisk&1)?"B":"A");
    InDisk=SelectDisk;
  } else {
    fprintf(stderr, "Disk %d Side %s Ejected",SelectDisk>>1,(SelectDisk&1)?"B":"A");
    InDisk=255;
  }
}

void FDS::FCEU_FDSSelect() {
  if (TotalSides==0) {
    fprintf(stderr, "Not FDS; can't select disk.");
    return;
  }

  if (InDisk!=255) {
    fprintf(stderr, "Eject disk before selecting.");
    return;
  }
  SelectDisk=((SelectDisk+1)%TotalSides)&3;
  fprintf(stderr, "Disk %d Side %c Selected",SelectDisk>>1,(SelectDisk&1)?'B':'A');
}

void FDS::FDSFix(int a) {
  if ((IRQa&2) && IRQCount) {
    IRQCount-=a;
    if (IRQCount<=0) {
      if (!(IRQa&1)) {
	IRQa&=~2;
	IRQCount=IRQLatch=0;
      }
      else {
	IRQCount=IRQLatch;
      }
      //IRQCount=IRQLatch; //0xFFFF;
      X.IRQBegin(FCEU_IQEXT);
      //printf("IRQ: %d\n",timestamp);
      //   printf("IRQ: %d\n",scanline);
    }
  }
  if (DiskSeekIRQ>0) {
    DiskSeekIRQ-=a;
    if (DiskSeekIRQ<=0) {
      if (FDSRegs[5]&0x80) {
	X.IRQBegin(FCEU_IQEXT2);
      }
    }
  }
}

static DECLFR(FDSRead4030) {
  uint8 ret = 0;

  /* Cheap hack. */
  TRACEF("FDSRead IRQlow %02x", X.IRQlow);
  if (X.IRQlow&FCEU_IQEXT) ret|=1;
  if (X.IRQlow&FCEU_IQEXT2) ret|=2;

  if (!fceuindbg) {
    X.IRQEnd(FCEU_IQEXT);
    X.IRQEnd(FCEU_IQEXT2);
  }
  return ret;
}

static DECLFR(FDSRead4031) {
  return fceulib__fds.FDSRead4013_Direct(DECLFR_FORWARD);
}

DECLFR_RET FDS::FDSRead4013_Direct(DECLFR_ARGS) {
  if (InDisk!=255) {
    fdsread4013_z=diskdata[InDisk][DiskPtr];
    if (!fceuindbg) {
      if (DiskPtr<64999) DiskPtr++;
      DiskSeekIRQ=150;
      X.IRQEnd(FCEU_IQEXT2);
    }
  }
  return fdsread4013_z;
}

static DECLFR(FDSRead4032) {
  return fceulib__fds.FDSRead4032_Direct(DECLFR_FORWARD);
}

DECLFR_RET FDS::FDSRead4032_Direct(DECLFR_ARGS) {
  uint8 ret;

  ret=X.DB&~7;
  if (InDisk==255)
    ret|=5;

  if (InDisk==255 || !(FDSRegs[5]&1) || (FDSRegs[5]&2))
    ret|=2;
  return ret;
}

static DECLFR(FDSRead4033) {
	return 0x80; // battery
}

static DECLFW(FDSRAMWrite) {
  return fceulib__fds.FDSRAMWrite_Direct(DECLFW_FORWARD);
}

void FDS::FDSRAMWrite_Direct(DECLFW_ARGS) {
  (FDSRAM-0x6000)[A]=V;
}

static DECLFR(FDSBIOSRead) {
  return fceulib__fds.FDSBIOSRead_Direct(DECLFR_FORWARD);
}

DECLFR_RET FDS::FDSBIOSRead_Direct(DECLFR_ARGS) {
  return (FDSBIOS-0xE000)[A];
}

static DECLFR(FDSRAMRead) {
  return fceulib__fds.FDSRAMRead_Direct(DECLFR_FORWARD);
}

DECLFR_RET FDS::FDSRAMRead_Direct(DECLFR_ARGS) {
  return (FDSRAM-0x6000)[A];
}

/* Begin FDS sound */

#define FDSClock (1789772.7272727272727272/2)

// XXX get rid of these
#define SPSG  fdso.SPSG
#define b19shiftreg60  fdso.b19shiftreg60
#define b24adder66  fdso.b24adder66
#define b24latch68  fdso.b24latch68
#define b17latch76  fdso.b17latch76
#define b8shiftreg88  fdso.b8shiftreg88
#define clockcount  fdso.clockcount
#define speedo    fdso.speedo

void FDS::FDSSoundStateAdd() {
  AddExState(fdso.cwave,64,0,"WAVE");
  AddExState(fdso.mwave,32,0,"MWAV");
  AddExState(fdso.amplitude,2,0,"AMPL");
  AddExState(SPSG,0xB,0,"SPSG");

  AddExState(&b8shiftreg88,1,0,"B88");

  AddExState(&clockcount, 4, 1, "CLOC");
  AddExState(&b19shiftreg60,4,1,"B60");
  AddExState(&b24adder66,4,1,"B66");
  AddExState(&b24latch68,4,1,"B68");
  AddExState(&b17latch76,4,1,"B76");
}

static DECLFR(FDSSRead) {
  return fceulib__fds.FDSSRead_Direct(DECLFR_FORWARD);
}

DECLFR_RET FDS::FDSSRead_Direct(DECLFR_ARGS) {
  switch(A&0xF) {
  case 0x0:return(fdso.amplitude[0]|(X.DB&0xC0));
  case 0x2:return(fdso.amplitude[1]|(X.DB&0xC0));
  }
  return(X.DB);
}

static DECLFW(FDSSWrite) {
  return fceulib__fds.FDSSWrite_Direct(DECLFW_FORWARD);
}

void FDS::FDSSWrite_Direct(DECLFW_ARGS) {
  if (FCEUS_SNDRATE) {
    if (FCEUS_SOUNDQ >= 1)
      RenderSoundHQ();
    else
      RenderSound();
  }
  A-=0x4080;
  switch(A) {
  case 0x0:
  case 0x4:
    if (V&0x80)
      fdso.amplitude[(A&0xF)>>2]=V&0x3F; //)>0x20?0x20:(V&0x3F);
    break;
  case 0x5://printf("$%04x:$%02x\n",A,V);
    break;
  case 0x7: b17latch76=0;SPSG[0x5]=0;//printf("$%04x:$%02x\n",A,V);
    break;
  case 0x8:
    b17latch76=0;
    //   printf("%d:$%02x, $%02x\n",SPSG[0x5],V,b17latch76);
    fdso.mwave[SPSG[0x5]&0x1F]=V&0x7;
    SPSG[0x5]=(SPSG[0x5]+1)&0x1F;
    break;
  }
  //if (A>=0x7 && A!=0x8 && A<=0xF)
  //if (A==0xA || A==0x9)
  //printf("$%04x:$%02x\n",A,V);
  SPSG[A]=V;
}

// $4080 - Fundamental wave amplitude data register 92
// $4082 - Fundamental wave frequency data register 58
// $4083 - Same as $4082($4083 is the upper 4 bits).

// $4084 - Modulation amplitude data register 78
// $4086 - Modulation frequency data register 72
// $4087 - Same as $4086($4087 is the upper 4 bits)


void FDS::DoEnv() {
  for (int x=0;x<2;x++) {
    if (!(SPSG[x<<2]&0x80) && !(SPSG[0x3]&0x40)) {
      if (counto[x]<=0) {
	if (!(SPSG[x<<2]&0x80)) {
	  if (SPSG[x<<2]&0x40) {
	    if (fdso.amplitude[x]<0x3F)
	      fdso.amplitude[x]++;
	  } else {
	    if (fdso.amplitude[x]>0)
	      fdso.amplitude[x]--;
	  }
	}
	counto[x]=(SPSG[x<<2]&0x3F);
      } else {
	counto[x]--;
      }
    }
  }
}

static DECLFR(FDSWaveRead) {
  return fceulib__fds.FDSWaveRead_Direct(DECLFR_FORWARD);
}
DECLFR_RET FDS::FDSWaveRead_Direct(DECLFR_ARGS) {
  return fdso.cwave[A&0x3f] | (X.DB&0xC0);
}

static DECLFW(FDSWaveWrite) {
  return fceulib__fds.FDSWaveWrite_Direct(DECLFW_FORWARD);
}
void FDS::FDSWaveWrite_Direct(DECLFW_ARGS) {
  //printf("$%04x:$%02x, %d\n",A,V,SPSG[0x9]&0x80);
  if (SPSG[0x9]&0x80)
    fdso.cwave[A&0x3f]=V&0x3F;
}

void FDS::ClockRise() {
  if (!clockcount) {
    ta++;

    b19shiftreg60=(SPSG[0x2]|((SPSG[0x3]&0xF)<<8));
    b17latch76=(SPSG[0x6]|((SPSG[0x07]&0xF)<<8))+b17latch76;

    if (!(SPSG[0x7]&0x80)) {
      int t=fdso.mwave[(b17latch76>>13)&0x1F]&7;
      int t2=fdso.amplitude[1];
      int adj = 0;

      if ((t&3)) {
	if ((t&4))
	  adj -= (t2 * ((4 - (t&3) ) ));
	else
	  adj += (t2 * ( (t&3) ));
      }
      adj *= 2;
      if (adj > 0x7F) adj = 0x7F;
      if (adj < -0x80) adj = -0x80;
      //if (adj) printf("%d ",adj);
      b8shiftreg88=0x80 + adj;
    } else {
      b8shiftreg88=0x80;
    }
  } else {
    b19shiftreg60<<=1;
    b8shiftreg88>>=1;
  }
  // b24adder66=(b24latch68+b19shiftreg60)&0x3FFFFFF;
  b24adder66=(b24latch68+b19shiftreg60)&0x1FFFFFF;
}

void FDS::ClockFall() {
  //if (!(SPSG[0x7]&0x80))
  {
    if ((b8shiftreg88&1)) // || clockcount==7)
      b24latch68=b24adder66;
  }
  clockcount=(clockcount+1)&7;
}

int32 FDS::FDSDoSound() {
  fdso.count+=fdso.cycles;
  if (fdso.count>=((int64)1<<40)) {
    // This is mysterious to me. We entire the loop if the count exceeds
    // 1<<40, but also goto directly into it below if it's greater than
    // 32768. Since we keep gotoing here, it looks equivalent to just
    // while fdso.count >= 32768 ?

    // XXX do { } while...
  dogk:
    fdso.count-=(int64)1<<40;
    ClockRise();
    ClockFall();
    fdso.envcount--;
    if (fdso.envcount<=0) {
      fdso.envcount+=SPSG[0xA]*3;
      DoEnv();
    }
  }
  if (fdso.count>=32768) goto dogk;

  // Might need to emulate applying the amplitude to the waveform a bit better...
  {
    int k=fdso.amplitude[0];
    if (k>0x20) k=0x20;
    return (fdso.cwave[b24latch68>>19]*k)*4/((SPSG[0x9]&0x3)+2);
  }
}

void FDS::RenderSound() {
  int32 end, start;
  int32 x;

  start=FBC;
  end=(SOUNDTS<<16)/fceulib__sound.soundtsinc;
  if (end<=start)
    return;
  FBC=end;

  if (!(SPSG[0x9]&0x80)) {
    for (x=start;x<end;x++) {
      uint32 t=FDSDoSound();
      t+=t>>1;
      t>>=4;
      fceulib__sound.Wave[x>>4]+=t; //(t>>2)-(t>>3); //>>3;
    }
  }
}

void FDS::RenderSoundHQ() {
  if (!(SPSG[0x9]&0x80)) {
    for (uint32 x=FBC;x<SOUNDTS;x++) {
      uint32 t=FDSDoSound();
      t+=t>>1;
      fceulib__sound.WaveHi[x]+=t; //(t<<2)-(t<<1);
    }
  }
  // nb: formatting made this look like it was in the 'if' above,
  // but it wasn't -tom7
  FBC = SOUNDTS;
}

void FDS::HQSync(int32 ts) {
  FBC = ts;
}

void FDS::FDSSound(int c) {
  RenderSound();
  FBC = c;
}

void FDS::FDS_ESI() {
  if (FCEUS_SNDRATE) {
    if (FCEUS_SOUNDQ>=1) {
      fdso.cycles=(int64)1<<39;
    } else {
      fdso.cycles=((int64)1<<40)*FDSClock;
      fdso.cycles/=FCEUS_SNDRATE *16;
    }
  }
  //  fdso.cycles=(int64)32768*FDSClock/(FCEUS_SNDRATE *16);
  SetReadHandler(0x4040,0x407f,FDSWaveRead);
  SetWriteHandler(0x4040,0x407f,FDSWaveWrite);
  SetWriteHandler(0x4080,0x408A,FDSSWrite);
  SetReadHandler(0x4090,0x4092,FDSSRead);

  //SetReadHandler(0xE7A3,0xE7A3,FDSBIOSPatch);
}

void FDS::FDSSoundReset() {
  memset(&fdso,0,sizeof(fdso));
  FDS_ESI();
  fceulib__sound.GameExpSound.HiSync = [](int32 ts){ return fceulib__fds.HQSync(ts); };
  fceulib__sound.GameExpSound.HiFill = []() { return fceulib__fds.RenderSoundHQ(); };
  fceulib__sound.GameExpSound.Fill = [](int i) { return fceulib__fds.FDSSound(i); };
  fceulib__sound.GameExpSound.RChange =  []() { return fceulib__fds.FDS_ESI(); };
}

static DECLFW(FDSWrite) {
  return fceulib__fds.FDSWrite_Direct(DECLFW_FORWARD);
}

void FDS::FDSWrite_Direct(DECLFW_ARGS) {
  //extern int scanline;
  //FCEU_printf("$%04x:$%02x, %d\n",A,V,scanline);
  switch(A) {
  case 0x4020:
    X.IRQEnd(FCEU_IQEXT);
    IRQLatch&=0xFF00;
    IRQLatch|=V;
    //  printf("$%04x:$%02x\n",A,V);
    break;
  case 0x4021:
    X.IRQEnd(FCEU_IQEXT);
    IRQLatch&=0xFF;
    IRQLatch|=V<<8;
    //  printf("$%04x:$%02x\n",A,V);
    break;
  case 0x4022:
    X.IRQEnd(FCEU_IQEXT);
    IRQCount=IRQLatch;
    IRQa=V&3;
    //  printf("$%04x:$%02x\n",A,V);
    break;
  case 0x4023:break;
  case 0x4024:
    if (InDisk!=255 && !(FDSRegs[5]&0x4) && (FDSRegs[3]&0x1)) {
      if (DiskPtr>=0 && DiskPtr<65500) {
	if (writeskip) writeskip--;
	else if (DiskPtr>=2) {
	  DiskWritten=1;
	  diskdata[InDisk][DiskPtr-2]=V;
	}
      }
    }
    break;
  case 0x4025:
    X.IRQEnd(FCEU_IQEXT2);
    if (InDisk!=255) {
      if (!(V&0x40)) {
	if (FDSRegs[5]&0x40 && !(V&0x10)) {
	  DiskSeekIRQ=200;
	  DiskPtr-=2;
	}
	if (DiskPtr<0) DiskPtr=0;
      }
      if (!(V&0x4)) writeskip=2;
      if (V&2) {DiskPtr=0;DiskSeekIRQ=200;}
      if (V&0x40) DiskSeekIRQ=200;
    }
    fceulib__cart.setmirror(((V>>3)&1)^1);
    break;
  }
  FDSRegs[A&7]=V;
}

void FDS::FreeFDSMemory() {
  for (int x=0; x<TotalSides; x++) {
    if (diskdata[x]) {
      free(diskdata[x]);
      diskdata[x]=0;
    }
  }
}

int FDS::SubLoad(FceuFile *fp) {
  struct md5_context md5;
  uint8 header[16];

  FCEU_fread(header,16,1,fp);

  if (memcmp(header,"FDS\x1a",4)) {
    if (!(memcmp(header+1,"*NINTENDO-HVC*",14))) {
      long t;
      t=FCEU_fgetsize(fp);
      if (t<65500)
	t=65500;
      TotalSides=t/65500;
      FCEU_fseek(fp,0,SEEK_SET);
    } else {
      return 0;
    }
  } else {
    TotalSides=header[4];
  }

  md5_starts(&md5);

  if (TotalSides>8) TotalSides=8;
  if (TotalSides<1) TotalSides=1;

  for (int x=0;x<TotalSides;x++) {
    diskdata[x]=(uint8 *)FCEU_malloc(65500);
    if (!diskdata[x]) {
      int zol;
      for (zol=0;zol<x;zol++)
	free(diskdata[zol]);
      return 0;
    }
    FCEU_fread(diskdata[x],1,65500,fp);
    md5_update(&md5,diskdata[x],65500);
  }
  md5_finish(&md5,GameInfo->MD5.data);
  return(1);
}

void FDS::PreSave() {
  //if (DiskWritten)
  for (int x=0;x<TotalSides;x++) {
    for (int b=0; b<65500; b++) {
      diskdata[x][b] ^= diskdatao[x][b];
    }
  }
}

void FDS::PostSave() {
  //if (DiskWritten)
  for (int x=0;x<TotalSides;x++) {

    for (int b=0; b<65500; b++) {
      diskdata[x][b] ^= diskdatao[x][b];
    }
  }
}

int FDS::FDSLoad(const char *name, FceuFile *fp) {
  FILE *zp;
  char *fn;

  FCEU_fseek(fp,0,SEEK_SET);

  if (!SubLoad(fp))
    return(0);


  // XXX use std::string -tom7
  fn = strdup(FCEU_MakeFDSFilename().c_str());

  if (!(zp=FCEUD_UTF8fopen(fn,"rb"))) {
    FCEU_PrintError("FDS BIOS ROM image missing: %s", 
		    FCEU_MakeFDSFilename().c_str());
    FreeFDSMemory();
    free(fn);
    return 0;
  }

  free(fn);

  fseek( zp, 0L, SEEK_END );
  if (ftell( zp ) != 8192 ) {
    fclose(zp);
    FreeFDSMemory();
    FCEU_PrintError("FDS BIOS ROM image incompatible: %s", 
		    FCEU_MakeFDSROMFilename().c_str());
    return 0;
  }
  fseek( zp, 0L, SEEK_SET );

  if (fread(FDSBIOS,1,8192,zp)!=8192) {
    fclose(zp);
    FreeFDSMemory();
    FCEU_PrintError("Error reading FDS BIOS ROM image.");
    return 0;
  }

  fclose(zp);

  if (!fceulib__cart.disableBatteryLoading) {
    FceuFile *tp;
    char *fn=strdup(FCEU_MakeFDSFilename().c_str());

    for (int x=0;x<TotalSides;x++) {
      diskdatao[x]=(uint8 *)FCEU_malloc(65500);
      memcpy(diskdatao[x],diskdata[x],65500);
    }

    if ((tp=FCEU_fopen(fn,"wb",0))) {
      FreeFDSMemory();
      if (!SubLoad(tp)) {
	FCEU_PrintError("Error reading auxillary FDS file.");
	free(fn);
	return(0);
      }
      FCEU_fclose(tp);
      DiskWritten=1;  /* For save state handling. */
    }
    free(fn);
  }

  GameInfo->type=GIT_FDS;
  GameInterface=FDSGI_Trampoline;

  SelectDisk=0;
  InDisk=255;

  ResetExState([]() { return fceulib__fds.PreSave(); }, 
	       []() { return fceulib__fds.PostSave(); });
  FDSSoundStateAdd();

  for (int x=0;x<TotalSides;x++) {
    char temp[5];
    sprintf(temp,"DDT%d",x);
    AddExState(diskdata[x],65500,0,temp);
  }

  AddExState(FDSRAM,32768,0,"FDSR");
  AddExState(FDSRegs,sizeof(FDSRegs),0,"FREG");
  AddExState(CHRRAM,8192,0,"CHRR");
  AddExState(&IRQCount, 4, 1, "IRQC");
  AddExState(&IRQLatch, 4, 1, "IQL1");
  AddExState(&IRQa, 1, 0, "IRQA");
  AddExState(&writeskip,1,0,"WSKI");
  AddExState(&DiskPtr,4,1,"DPTR");
  AddExState(&DiskSeekIRQ,4,1,"DSIR");
  AddExState(&SelectDisk,1,0,"SELD");
  AddExState(&InDisk,1,0,"INDI");
  AddExState(&DiskWritten,1,0,"DSKW");

  fceulib__cart.ResetCartMapping();
  fceulib__cart.SetupCartCHRMapping(0,CHRRAM,8192,1);
  fceulib__cart.SetupCartMirroring(0,0,0);
  memset(CHRRAM,0,8192);
  memset(FDSRAM,0,32768);

  FCEU_printf(" Sides: %d\n\n",TotalSides);

  FCEUI_SetVidSystem(0);

  return 1;
}

void FDS::FDSClose() {
  FILE *fp;

  if (!DiskWritten) return;

  const std::string fn = FCEU_MakeFDSFilename();
  if (!(fp=FCEUD_UTF8fopen(fn.c_str(),"wb"))) {
    return;
  }

  for (int x=0;x<TotalSides;x++) {
    if (fwrite(diskdata[x],1,65500,fp)!=65500) {
      FCEU_PrintError("Error saving FDS image!");
      fclose(fp);
      return;
    }
  }
  FreeFDSMemory();
  fclose(fp);
}

FDS fceulib__fds;
