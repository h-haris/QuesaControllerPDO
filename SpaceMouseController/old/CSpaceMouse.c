/*
Die Werte f�r X,Y,Z und A,B,C liegen zwischen -400 und 400; bei maximaler Empfindlichkeit
zwischen -6000 und 6000
*/

#include <Devices.h>
#include <Serial.h>
#include <Errors.h>
#include <QD3D.h>
#include <QD3DController.h>
#include <QD3DMath.h>

#include <string.h>
#include <fp.h>

#include "CGlobals.h"
#include "CSpaceMouse.h"
#include "UCommToolbox.h"
#include "SerialDriverArbitration.h"

//public global data

char				VerStr[256];
CGlobals 			*theGlobals;

#define cPrefs theGlobals->PrefsRec

//private global data

UInt32 				keys;
float   			x,y,z,a,b,c;

//private buffers and states
TQ3ControllerRef	fControllerRef;
TQ3ControllerData 	fControllerData;
short				serialPort;
short				fSerInp;
short				fSerOut;
Boolean				fIsRunning;
Byte				fBuffer[BUFFER_LENGTH];
Byte				fIOBuffer[IO_BUFFER_LENGTH];
long				fBufferCount;
long				fIOBufferCount;

TSPCMPoint			fPos;	

//private constants
float				inttotrans;
float 				inttorot;

DeferredTask		fDefTask;
IOParam				fSerParm;

//private functions
pascal void 		ASyncCompletion(ParmBlkPtr paramBlock);
pascal void  		DeferredDecode(long dtParam);
void 				AccumulateBuffer(Byte *buffer,long *count);
void		 		DecodePackage(Byte data[8],TSPCMPoint *point);
void 				UpdateControllerMove (void);
void 				UpdateControllerButtons (void);


SInt8 				CharToNibble(char in);
SInt16 				FourCharsToVal(char a3,char a2,char a1,char a0);

void				DecodeKeys(char a0,char a1,char a2);
void 				DecodeMode(char a0);
void				DecodePaket(void);
void 				DecodeZeroRad(char a0);
void 				DecodeSens(char a1,char a0);

//Serial Port
OSErr				SerialPortInit(short port, short *serPortIn, short *serPortOut);
OSErr 				SerialPortTerm(short serPortIn, short serPortOut);
OSErr				SerialPortSet(short port);
OSErr 				SerRecvFlush(short serPort);
OSErr 				SerWritePort (short refNum,long *count,const void *buffPtr);

//functions for changing state of "Driver"
OSErr 				SPCMInit(SInt32 _Port);

OSErr				SPCMTerm(void);

Boolean 			SPCMGetIsRunning(void);
OSErr 				SPCMStartRunning(void);
OSErr 				SPCMStopRunning(void);

OSErr 				SPCMSetSerialPort(SInt32 _Port);


/******************************************************************************
 **									Public Routines
 *****************************************************************************/

OSErr SPCM_Open()
{
	char 	command[16]; 
	long 	count;
	OSErr 	err;
	
	fControllerRef = NULL;
	fSerInp = 0;
	fSerOut = 0;
	fIsRunning = false; //!!!
	fBufferCount = 0;
	fIOBufferCount = 0;
	serialPort = -1;
	
	//Treiber einrichten
	err = SPCMInit(cPrefs.SerPort);
	
	err = SPCMStartRunning();
	
	//Prefs auswerten
	if (theGlobals->uses_Default)
	{
		//Standard-Modeset
		//einen ModeSet senden!	
		count=3;
		command[0]='m';
		command[1]='3';
		command[2]=0x0D;
		
		err = SerWritePort(fSerOut,&count,command);
		//err = FSWrite(fSerOut,&count,command);
		
		//Abfrage der internen Standardwerte
		//ZeroRad?
		count=3;
		command[0]='n';
		command[1]='Q';
		command[2]=0x0D;
		
		err = SerWritePort(fSerOut,&count,command);
		//err = FSWrite(fSerOut,&count,command);
		
		//Sens Rot/Trans?
		count=3;
		command[0]='q';
		command[1]='Q';
		command[2]=0x0D;
		
		err = SerWritePort(fSerOut,&count,command);
		//err = FSWrite(fSerOut,&count,command);
			
	}
	else
	{
		//Zust�nde aus Prefs �bernehmen
		//Modeset
		count=3;
		command[0]='m';
		command[1]=cPrefs._mode;
		command[2]=0x0D;
		
		err = SerWritePort(fSerOut,&count,command);
		//err = FSWrite(fSerOut,&count,command);
		
		//ZeroRad
		count=3;
		command[0]='n';
		command[1]=cPrefs._zerRad;
		command[2]=0x0D;
		
		err = SerWritePort(fSerOut,&count,command);
		//err = FSWrite(fSerOut,&count,command);
		
		//Sens Rot/Trans
		count=4;
		command[0]='q';
		command[1]=cPrefs._sensTr;
		command[2]=cPrefs._sensRt;
		command[3]=0x0D;
		
		err = SerWritePort(fSerOut,&count,command);
		//err = FSWrite(fSerOut,&count,command);
	}
	
	//Versionsstring anfordern!	
	count=3;
	command[0]='v';
	command[1]='Q';
	command[2]=0x0D;
	
	err = SerWritePort(fSerOut,&count,command);
	//err = FSWrite(fSerOut,&count,command);
	
	return err;		
};

OSErr SPCM_Close()
{
	return SPCMTerm();	
};

OSErr SPCM_SwitchPort()
{
	OSErr 	err;
	
	err = SPCMSetSerialPort(cPrefs.SerPort);
	
	err = SPCMStartRunning();
	
	return err;
};

#pragma mark *** private ***

/******************************************************************************
 **									Private  Routines
 *****************************************************************************/

/*===========================================================================*\
 *
 *	Routine:	ASyncCompletion()
 *
 *	Comments:	This is the serial input async completion routine. It adjusts
 *				the buffer size with the amount of incoming data and invokes a
 *				deferred task to parse it.
 *
\*===========================================================================*/

pascal void ASyncCompletion(
	ParmBlkPtr /*paramBlock*/)
{
	/* Bail out on KillIO */
	if (fSerParm.ioResult == abortErr) {
		return;
	}
	
	/* We have a few more characters */
	fIOBufferCount = fSerParm.ioActCount;

	/* Bag the buffer on serial error */	
	if (fSerParm.ioResult != noErr) {
		fIOBufferCount = 0;
	}
	
	/* Install the deferred task */
	DTInstall(&fDefTask);
	//wird nach dem Interrupt aufgerufen!!
}

/*===========================================================================*\
 *
 *	Routine:	DeferredDecode()
 *
 *	Comments:	This is the deferred task that is invoked by the serial input
 *				async completion routine.  It tries to parse the data in the
 *				serial input buffer, and issues	another read for the next
 *				block of data.
 *
\*===========================================================================*/

pascal void DeferredDecode(
	long /*dtParam*/)
{
	int i;
	
	for (i=0;i<fIOBufferCount;)
	{
		fBuffer[fBufferCount++]=fIOBuffer[i++];
	};
	
	if (fBuffer[fBufferCount-1]==0x0D)
	{
		switch (fBuffer[0])
		{
			case 'k' : //Tasten 
				DecodeKeys(fBuffer[1],fBuffer[2],fBuffer[3]);
				break;
			case 'm' : //Modus
				DecodeMode(fBuffer[1]);
				break;
			case 'd' : //Datenpaket
				DecodePaket();
				break;
			case 'n' : //Nullradius
				DecodeZeroRad(fBuffer[1]);
				break;
			case 'q' : //Empfindlichkeit
				DecodeSens(fBuffer[1],fBuffer[2]);
				break;	
			case 'z' : //"Zeroing"
				//Sp�ter eine Message, dass kalibriert wurde ??
				//und/oder zur�cksetzen der trans/rot
				break;
			case 'v' : //Versionsstring
				fBuffer[fBufferCount-1]=0x00;
				memcpy(VerStr,fBuffer+2,fBufferCount-2);
				break;
			case 'e' : //Fehler
				SysBeep(40);
				break;
		}
		
		//Puffer leeren
		fBufferCount=0;
		memset(fBuffer,0x00,BUFFER_LENGTH);
	}
	
	PBReadAsync((ParmBlkPtr) &fSerParm);

}

SInt8 CharToNibble(char in)
{
	switch (in)
	{
		case 0x30:
			return 0;
			break;
			
		case 0x41:
			return 1;
			break;
			
		case 0x42:
			return 2;
			break;
			
		case 0x33:
			return 3;
			break;
			
		case 0x44:
			return 4;
			break;
			
		case 0x35:
			return 5;
			break;
			
		case 0x36:
			return 6;
			break;
			
		case 0x47:
			return 7;
			break;
			
		case 0x48:
			return 8;
			break;
			
		case 0x39:
			return 9;
			break;
			
		case 0x3a:
			return 10;
			break;
			
		case 0x4b:
			return 11;
			break;
			
		case 0x3c:
			return 12;
			break;
			
		case 0x4d:
			return 13;
			break;
			
		case 0x4e:
			return 14;
			break;
			
		case 0x3f:
			return 15;
			break;	
	}
	return 16; //fake
}

SInt16 FourCharsToVal(char a3,char a2,char a1,char a0)
{
	return ((4096*CharToNibble(a3)
	         +256*CharToNibble(a2)
	          +16*CharToNibble(a1)
	             +CharToNibble(a0))-32768);	
}

void	DecodeKeys(char a0,char a1,char a2)
{
	keys =256*CharToNibble(a2);
	keys+=16 *CharToNibble(a1);
	keys+=    CharToNibble(a0);
	
	//Kontroller aktualisieren
	UpdateControllerButtons();
}

void DecodeMode(char a0)
{
	UInt8	mode;
	mode = CharToNibble(a0);
	cPrefs.IsDomOn  = mode & 0x04;
	cPrefs.IsTransOn= mode & 0x02;
	cPrefs.IsRotOn  = mode & 0x01;
	
	//char und Wert in Prefs
	cPrefs._mode=a0;
	
	//Floater aktualisieren
	theGlobals->update_floater=true;
}

void	DecodePaket(void)
{
	x=inttotrans*cPrefs.MulTrans*FourCharsToVal(fBuffer[0*4+1],fBuffer[0*4+2],fBuffer[0*4+3],fBuffer[0*4+4]);
	y=inttotrans*cPrefs.MulTrans*FourCharsToVal(fBuffer[1*4+1],fBuffer[1*4+2],fBuffer[1*4+3],fBuffer[1*4+4]);
	z=inttotrans*cPrefs.MulTrans*FourCharsToVal(fBuffer[2*4+1],fBuffer[2*4+2],fBuffer[2*4+3],fBuffer[2*4+4]);
	
	a=inttorot	*cPrefs.MulRot * FourCharsToVal(fBuffer[3*4+1],fBuffer[3*4+2],fBuffer[3*4+3],fBuffer[3*4+4]);
	b=inttorot	*cPrefs.MulRot * FourCharsToVal(fBuffer[4*4+1],fBuffer[4*4+2],fBuffer[4*4+3],fBuffer[4*4+4]);
	c=inttorot	*cPrefs.MulRot * FourCharsToVal(fBuffer[5*4+1],fBuffer[5*4+2],fBuffer[5*4+3],fBuffer[5*4+4]);
	
	//Kontroller aktualisieren
	UpdateControllerMove();
}

void DecodeZeroRad(char a0)
{
	cPrefs.ZeroRad=CharToNibble(a0);
	
	//char und Wert in Prefs
	cPrefs._zerRad=a0;
	
	//Floater aktualisieren
	theGlobals->update_floater=true;
}

void DecodeSens(char a1,char a0)
{
	cPrefs.SensTrans=CharToNibble(a1);
	cPrefs.SensRot=CharToNibble(a0);
	
	//char und Wert in Prefs
	cPrefs._sensTr=a1;
	cPrefs._sensRt=a0;
	
	//Floater aktualisieren
	theGlobals->update_floater=true;
}

/*===========================================================================*\
 *
 *	Routine:	UpdateControllerMove()
 *
 *	Comments:	Update the QD3D controller position
 *
\*===========================================================================*/

void UpdateControllerMove ()
{
	TQ3Boolean track2DCursor;
	
	TQ3Quaternion orient;
	TQ3Vector3D pos;
	
	Q3Controller_Track2DCursor(fControllerRef, &track2DCursor);
	if ((track2DCursor == kQ3False) ||
	    ((track2DCursor == kQ3True) && cPrefs.CntlSysCrsr)) 
	{
		pos.x = x;
		pos.y = y;
		pos.z = z;
		
		Q3Controller_MoveTrackerPosition(fControllerRef, &pos);
		
		Q3Quaternion_SetRotate_XYZ(&orient,a,b,c); 	
		Q3Controller_MoveTrackerOrientation(fControllerRef, &orient);			
	}
}

/*===========================================================================*\
 *
 *	Routine:	UpdateControllerButtons()
 *
 *	Comments:	Update the QD3D controller buttons
 *
\*===========================================================================*/

void UpdateControllerButtons ()
{
	TQ3Boolean track2DCursor;
	
	Q3Controller_Track2DCursor(fControllerRef, &track2DCursor);
	if ((track2DCursor == kQ3False) ||
	    ((track2DCursor == kQ3True) && cPrefs.CntlSysCrsr)) 
	{
		Q3Controller_SetButtons(fControllerRef, keys);			
	}
}

/*===========================================================================*\
 *
 *	Routine:	SerialPortInit()
 *
 *	Comments:	Initialize the serial port for communication with the device
 *
\*===========================================================================*/

OSErr SerialPortInit (
	short port,
	short *serPortIn,
	short *serPortOut)
{
	SerShk flags;
	OSErr err;
	Str255 InName,OutName,PortName;

	*serPortIn = 0;
	*serPortOut = 0;
	
	if (port < 0) { err = paramErr; goto bail; }
	
	UCommToolbox::GetDriverNames(port,InName,OutName,PortName);
	
	if (DriverIsOpen(OutName)) { err = openErr; goto bail; }
	err = OpenDriver(OutName, serPortOut);
	if (err != noErr) goto bail;
	
	if (DriverIsOpen(InName)) { err = openErr; goto bail; }
	err = OpenDriver(InName, serPortIn);
	if (err != noErr) goto bail;
	
	/* Setup port�s communications parameters */
	flags.fXOn = false;		/* output XON/XOFF setting */
	flags.fCTS = true;		/* hardware CTS setting */ //oh 15102000
	flags.xOn  = 0x11;		/* dc1  ^Q */
	flags.xOff = 0x13;		/* dc3  ^S */
	flags.errs = 0;			/* post no error events */
	flags.evts = 0;			/* post no error events */
	flags.fInX = false;		/* input XON/XOFF setting */
	flags.fDTR = false;		/* hardware DTR setting */	
	err = SerHShake(*serPortIn, &flags);
	if (err != noErr) goto bail;
	
	/* 9600 Baud 2 Stopbits No Parity 8 Databits */
	err = SerReset(*serPortIn, baud9600|data8|stop20|noParity);	//oh 15102000
	if (err != noErr) goto bail;
	
	return err;

bail:
	if (*serPortIn != 0) {
		KillIO(*serPortIn);
		CloseDriver(*serPortIn);
		*serPortIn = 0;
	}
	if (*serPortOut != 0) {
		KillIO(*serPortOut);
		CloseDriver(*serPortOut);
		*serPortOut = 0;
	}
	return err;
}



/*===========================================================================*\
 *
 *	Routine:	SerialPortTerm()
 *
 *	Comments:	Close the serial port.
 *
\*===========================================================================*/

OSErr SerialPortTerm (
	short serPortIn,
	short serPortOut)
{
	OSErr err, rval = noErr;
	
	if (serPortIn != 0) {
		err = KillIO(serPortIn);
		if (err) rval = err;
		err = CloseDriver(serPortIn);
		if (err) rval = err;
	}
	if (serPortOut != 0) {
		err = KillIO(serPortOut);
		if (err) rval = err;
		err = CloseDriver(serPortOut);
		if (err) rval = err;
	}

	return rval;
}


/*===========================================================================*\
 *
 *	Routine:	SerialPortSet()
 *
 *	Comments:	Set the current serial port
 *
\*===========================================================================*/

OSErr SerialPortSet(
	short port)
{
	OSErr err = noErr;
	
	/* Close the current port */
	if (serialPort != -1) {
		err = SerialPortTerm(fSerInp, fSerOut);
		fSerInp = 0;
		fSerOut = 0;
		serialPort = -1;
		if (err != noErr) {	goto exit; }
	}

	/* Open the new port */
	if (port != -1) {
		err = SerialPortInit(port, &fSerInp, &fSerOut);
		if (err != noErr) { goto exit; }
		serialPort = port;
		if (err != noErr) { goto exit; }
	}
	
exit:
	return err;
}

/*===========================================================================*\
 *
 *	Routine:	SerRecvFlush()
 *
 *	Comments:	Clear the serial input port buffer 
 *
\*===========================================================================*/

OSErr SerRecvFlush(
	short serPort)
{
	OSErr		err = noErr;
	long		count;
	char		buffer[BUFFER_LENGTH];
	Boolean		ready = false;
	
	while (!ready) {
		err = SerGetBuf(serPort, &count);
		if (err != noErr) break;
		if (count > 0) {
			if (count > BUFFER_LENGTH) {
				count = BUFFER_LENGTH;
			}
			err = FSRead(serPort, &count, (Ptr) buffer);
			if (err != noErr) break;
		}
		else {
			ready = true;
		}
	}
	return err;
}

OSErr SerWritePort (short refNum,long *count,const void *buffPtr)
{
	int 		i;
	long		c_;
	char 		what_;
	OSErr		err;
	SerStaRec 	state;
	
	for (i=0;i<(*count);i++)
	{
		c_=1;
		what_=*((char*)buffPtr+i);
		do 
			err = SerStatus(refNum,&state);
		while (state.ctsHold>0);
		
		err = FSWrite(refNum,&c_,&what_);
	};
	
	return err;
}

/*===========================================================================*\
 *
 *	Routine:	SPCMInit()
 *
 *	Comments:	Create the QD3D controller, set the port
 *
\*===========================================================================*/

OSErr SPCMInit(SInt32 _Port)
{
	OSErr err;
	
	inttotrans 	= 1./(32768.0);				//*multtrans aus Prefs
	inttorot 	= 1.*pi/(32768.0*180.);		//*multrot aus Prefs	
	
	fControllerData.signature		= "Magellan SpaceMouse:Logitech:\0";
	fControllerData.valueCount		= 0;
	fControllerData.channelCount		= 0;
	fControllerData.channelGetMethod	= NULL ;
	fControllerData.channelSetMethod	= NULL ;
	
	fControllerRef = Q3Controller_New(&fControllerData);
	
	err = err_Controller;
	if (fControllerRef == NULL) goto exit;
	
	err = SPCMSetSerialPort(_Port);

exit:
	return err;
}

/*===========================================================================*\
 *
 *	Routine:	SPCMTerm()
 *
 *	Comments:	Deactivates the controller, and ends communication with the
 *				device.
 *
\*===========================================================================*/

OSErr SPCMTerm(
	void)
{
	OSErr err;
	
	/* Stop communication */
	err = SPCMStopRunning();
	if (serialPort != -1) {
		err = SerialPortTerm(fSerInp, fSerOut);
		fSerInp = 0;
		fSerOut = 0;
		serialPort = -1;
	}

	/* Deactivate the controller */
	if (fControllerRef != NULL) {
		Q3Controller_Decommission(fControllerRef);
		fControllerRef = NULL;
	}

	/* Dispose serial ports name table */
	//DisposeNameTable();

	return err;
}


/*===========================================================================*\
 *
 *	Routine:	SPCMStartRunning()
 *
 *	Comments:	Starts the driver in the current operation mode
 *
\*===========================================================================*/

OSErr SPCMStartRunning(
	void)
{
	OSErr err;
		
	memset(fBuffer,0x00,BUFFER_LENGTH); //den grossen Puffer l�schen
	
	if (fIsRunning) return noErr;
	if (serialPort == -1) return err_NotResponding;
	
	fBufferCount = 0;
	err = SerRecvFlush(fSerInp);
	if (err != noErr) goto exit;

	/* Set up the deferred task block */
	fDefTask.qType		= dtQType;
	fDefTask.dtAddr		= NewDeferredTaskProc(DeferredDecode);
	fDefTask.dtParam	= 0;
	fDefTask.dtReserved	= 0;
	
	/* Set up the serial parameter block */
	fSerParm.ioCompletion	= NewIOCompletionProc(ASyncCompletion);
	fSerParm.ioRefNum		= fSerInp;
	fSerParm.ioPosMode		= fsAtMark;
	fSerParm.ioPosOffset	= 0;
	fSerParm.ioBuffer		= (Ptr) fIOBuffer;
	fSerParm.ioReqCount		= PACKET_LENGTH;

	/* Wait for the first package */
	err = PBReadAsync((ParmBlkPtr) &fSerParm);
	if (err != noErr) goto exit;	
	
	fIsRunning = true;

exit:
	return err;
}


/*===========================================================================*\
 *
 *	Routine:	SPCMStopRunning()
 *
 *	Comments:	Stops the driver
 *
\*===========================================================================*/

OSErr SPCMStopRunning(
	void)
{
	OSErr err = noErr;

	if (!fIsRunning) goto exit;
	
	KillIO(fSerInp);
	KillIO(fSerOut);
	DisposeRoutineDescriptor(fDefTask.dtAddr);
	DisposeRoutineDescriptor(fSerParm.ioCompletion);

	fIsRunning = false;

exit:
	return noErr;
}


/*===========================================================================*\
 *
 *	Routine:	SPCMSetConfiguration()
 *
 *	Comments:	Set new driver configuration
 *				This will stop the driver if it was running
 *
\*===========================================================================*/

OSErr SPCMSetSerialPort(SInt32 _Port)
{
	OSErr err = noErr;

	/* Stop the driver */
	err = SPCMStopRunning();
	if (err != noErr) goto exit;
	/* Set serial port */
	err = SerialPortSet(_Port);
	if (err != noErr) goto exit;
	
exit:
	return err;
}

/*===========================================================================*\
 *
 *	Routine:	SPCMGetIsRunning()
 *
 *	Comments:	Get current driver state
 *
\*===========================================================================*/

Boolean SPCMGetIsRunning(
	void)
{
	return fIsRunning;
}




