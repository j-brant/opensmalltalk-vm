/****************************************************************************
*   PROJECT: Mac window, memory, keyboard interface.
*   FILE:    sqMacDragDrop.c
*   CONTENT: 
*
*   AUTHOR:  John McIntosh, and others.
*   ADDRESS: 
*   EMAIL:   johnmci@smalltalkconsulting.com
*   RCSID:   $Id: sqMacDragDrop.c,v 1.3 2002/01/09 06:42:12 johnmci Exp $
*
*   NOTES: See change log below.
*	1/4/2002   JMM Carbon cleanup
*
*****************************************************************************/
/*	Drag and drop support for Squeak 
	John M McIntosh of Corporate Smalltalk Consulting Ltd
	johnmci@smalltalkconsulting.com 
	http://www.smalltalkconsulting.com 
	In Jan of 2001 under contract to Disney
	
	Dragging is only for objects into Squeak, not from Squeak outwards.
		
    V1.0 Jan 24th 2001, JMM
    V3.0.19 Aug 2001, JMM make a proper plugin

Some of this code comes from
	Author:		John Montbriand
				Some techniques borrowed from Pete Gontier's original FinderDragPro.


	Copyright: 	Copyright: � 1999 by Apple Computer, Inc.
				all rights reserved.
	
	Disclaimer:	You may incorporate this sample code into your applications without
				restriction, though the sample code has been provided "AS IS" and the
				responsibility for its operation is 100% yours.  However, what you are
				not permitted to do is to redistribute the source as "DSC Sample Code"
				after having made changes. If you're going to re-distribute the source,
				we require that you make it clear in the source that the code was
				descended from Apple Sample Code, but that you've made changes.
	
	Change History (most recent first):
	9/9/99 by John Montbriand
	
*/
/*
    need get filetype/creator
    need DropPlugin_shutdownModule & dropShutdown
    
*/

#include "sq.h"

#include <drag.h>
#include <macwindows.h>
#include <gestalt.h>
#include <quickdraw.h>


#include "sqVirtualMachine.h"
#include "FilePlugin.h"
#include "sqMacFileLogic.h"	

#include "DropPlugin.h"

	/* promise flavor types */
	
enum {
	kPromisedFlavor = 'fssP',		/* default promise */
	kPromisedFlavorFindFile = 'rWm1' /* Find File promise -- special case */
};


 Boolean gHasDragManager = false;                    /* true if the Drag Manager is installed */
 Boolean gCanTranslucentDrag = false;                /* true if translucent dragging is available */
 DragReceiveHandlerUPP gMainReceiveHandler = NULL;   /* receive handler for the main dialog */
 DragTrackingHandlerUPP gMainTrackingHandler = NULL; /* tracking handler for the main dialog */
 WindowPtr   gWindowPtr;
 
 UInt16 gNumDropFiles=0;
 HFSFlavor *dropFiles;

#define DOCUMENT_NAME_SIZE 1000
char tempName[DOCUMENT_NAME_SIZE + 1];  

	/* these routines are used both in the receive handler and inside of the
		tracking handler.  The following variables are shared between MyDragTrackingHandler
		and MyDragReceiveHandler.  */
		
 Boolean gApprovedDrag = false;   /* set to true if the drag is approved */
 Boolean gInIconBox = false;      /* set true if the drag is inside our drop box */
 long gSecurityCallbackToDisableSecurity = 0;
 
extern struct VirtualMachine *interpreterProxy;
 pascal OSErr MyDragTrackingHandler(DragTrackingMessage message, WindowPtr theWindow, void *refCon, DragReference theDragRef);
 pascal OSErr MyDragReceiveHandler(WindowPtr theWindow, void *refcon, DragReference theDragRef);

// Startup logic

int dropInit(void)
{
    long response,fn;
    Boolean  installedReceiver=false, installedTracker=false;
    OSErr err;
    
    /* check for the drag manager & translucent feature??? */
    
	if (gMainReceiveHandler != NULL) return 1;
	
	if (Gestalt(gestaltDragMgrAttr, &response) != noErr) return 0;
	
	gHasDragManager = (((1 << gestaltDragMgrPresent)) != 0);
	gCanTranslucentDrag = (((1 << gestaltDragMgrHasImageSupport)) != 0);

    if (!(gHasDragManager && gCanTranslucentDrag)) return 0;
	
	if ((Ptr)InstallTrackingHandler==(Ptr)kUnresolvedCFragSymbolAddress) return 0;
	
	fn = interpreterProxy->ioLoadFunctionFrom("getSTWindow", "");
	if (fn == 0) {
	    goto bail; 
	}
	gWindowPtr = (WindowPtr) ((int (*) ()) fn)();

	gMainTrackingHandler = NewDragTrackingHandlerUPP(MyDragTrackingHandler);
	if (gMainTrackingHandler == NULL) return 0;
	gMainReceiveHandler = NewDragReceiveHandlerUPP(MyDragReceiveHandler);
	if (gMainReceiveHandler == NULL) return 0;

		/* install the drag handlers, don't forget to dispose of them later */
		
	err = InstallTrackingHandler(gMainTrackingHandler, gWindowPtr, NULL);
	if (err != noErr) { 
	    err = memFullErr; 
	    goto bail; 
    }
	installedTracker = true;
	err = InstallReceiveHandler(gMainReceiveHandler, gWindowPtr, NULL);
	
	if (err != noErr) { 
	    err = memFullErr; 
	    goto bail; 
	}
	installedReceiver = true;
	return 1;
	
bail: 
    if (installedReceiver)
		RemoveReceiveHandler(gMainReceiveHandler, gWindowPtr);
	if (installedTracker)
		RemoveTrackingHandler(gMainTrackingHandler, gWindowPtr);
	
	gMainTrackingHandler = NULL; 
    gMainReceiveHandler = NULL;
    
	return 0;
}	

// Shutdown logic

int dropShutdown() {
    if (gMainReceiveHandler != NULL)
		RemoveReceiveHandler(gMainReceiveHandler, gWindowPtr);
	if (gMainTrackingHandler != NULL)
		RemoveTrackingHandler(gMainTrackingHandler, gWindowPtr);
	if (gNumDropFiles != 0 ) {
	    DisposePtr((char *) dropFiles);
	    gNumDropFiles = 0;
	}
	
	gMainTrackingHandler = NULL; 
    gMainReceiveHandler = NULL;

}

int sqSetFileAccessCallback(void *function) {
    gSecurityCallbackToDisableSecurity  = (long) function;
}

/*	Return a pointer to the first byte of of the file record within the given Smalltalk object, or nil if objectPointer is not a file record. */

static SQFile * fileValueOf(int objectPointer) {
	if (!((interpreterProxy->isBytes(objectPointer)) && ((interpreterProxy->byteSizeOf(objectPointer)) == ( sizeof(SQFile))))) {
		interpreterProxy->primitiveFail();
		return null;
	}
	return interpreterProxy->firstIndexableField(objectPointer);
}

//Primitive to get file name

char *dropRequestFileName(int dropIndex) {
    if(dropIndex < 1 || dropIndex > gNumDropFiles) 
        return NULL;
    PathToFile(tempName, 
        DOCUMENT_NAME_SIZE,
        &dropFiles[dropIndex-1].fileSpec);
  return tempName;
}

//Primitive to get file stream handle.

int dropRequestFileHandle(int dropIndex) {
    int  fileHandle,fn,iHFAfn;
    int  size,classPointer;
    char *dropName = dropRequestFileName(dropIndex);
    Boolean needToFlipFlagBack=false;
    
    if(!dropName)
        return interpreterProxy->nilObject();
 
    size = sizeof(SQFile);
    classPointer = interpreterProxy->classByteArray();
    fileHandle = interpreterProxy->instantiateClassindexableSize(classPointer, size);
	fn = interpreterProxy->ioLoadFunctionFrom("fileOpennamesizewrite", "FilePlugin");
	if (fn == 0) {
		/* begin primitiveFail */
        interpreterProxy->success(false);
		return null;
	}
	if (gSecurityCallbackToDisableSecurity != 0 ) {
    	iHFAfn = interpreterProxy->ioLoadFunctionFrom("secHasFileAccess", "SecurityPlugin");
    	if ((iHFAfn != 0) && !((int (*) (void)) iHFAfn)()){
 	    	((int (*) (int)) gSecurityCallbackToDisableSecurity)(true);
    	    needToFlipFlagBack = true;
    	} 
    }
	((int (*) (SQFile *, int, int,int)) fn)(fileValueOf(fileHandle),(int)dropName, strlen(dropName), 0);
    if (needToFlipFlagBack)
 	    ((int (*) (int)) gSecurityCallbackToDisableSecurity)(false);
    
  return fileHandle;
}


/* RECEIVING DRAGS ------------------------------------------------ */


/* ApproveDragReference is called by the drag tracking handler to determine
	if the contents of the drag can be handled by our receive handler.

	Note that if a flavor can't be found, it's not really an
	error; it only means the flavor wasn't there and we should
	not accept the drag. Therefore, we translate 'badDragFlavorErr'
	into a 'false' value for '*approved'. */
	
static pascal OSErr ApproveDragReference(DragReference theDragRef, Boolean *approved) {

	OSErr err;
	DragAttributes dragAttrs;
	FlavorFlags flavorFlags;
	ItemReference theItem;
		
		/* we cannot drag to our own window */
	if ((err = GetDragAttributes(theDragRef, &dragAttrs)) != noErr) 
	    goto bail;
	    
	if ((dragAttrs & kDragInsideSenderWindow) != 0) { 
	    err = userCanceledErr; 
	    goto bail; 
    }
	
		/* gather information about the drag & a reference to item one. */
	if ((err = GetDragItemReferenceNumber(theDragRef, 1, &theItem)) != noErr) 
	    goto bail;
		
		/* check for flavorTypeHFS */
	err = GetFlavorFlags(theDragRef, theItem, flavorTypeHFS, &flavorFlags);
	if (err == noErr) {
		*approved = true;
		return noErr;
	} else if (err != badDragFlavorErr)
		goto bail;
		
		/* check for flavorTypePromiseHFS */
	err = GetFlavorFlags(theDragRef, theItem, flavorTypePromiseHFS, &flavorFlags);
	if (err == noErr) {
		*approved = true;
		return noErr;
	} else if (err != badDragFlavorErr)
		goto bail;
		
		/* none of our flavors were found */
	*approved = false;
	return noErr;
	
bail:
		/* an error occured, clean up.  set result to false. */
	*approved = false;
	return err;
}



/* MyDragTrackingHandler is called for tracking the mouse while a drag is passing over our
	window.  if the drag is approved, then the drop box will be hilitied appropriately
	as the mouse passes over it.  */

pascal OSErr MyDragTrackingHandler(DragTrackingMessage message, WindowPtr theWindow, void *refCon, DragReference theDragRef) {
		/* we're drawing into the image well if we hilite... */
    Rect  bounds;
	EventRecord		theEvent;
    int     fn;

	switch (message) {
	
		case kDragTrackingEnterWindow:
			{	
				Point mouse;
				
				gApprovedDrag = false;
				if (theWindow == gWindowPtr) {
					if (ApproveDragReference(theDragRef, &gApprovedDrag) != noErr) break;
					if ( ! gApprovedDrag ) break;
					SetPortWindowPort(theWindow);
					GetMouse(&mouse);
					GetPortBounds(GetWindowPort(gWindowPtr),&bounds);
					if (PtInRect(mouse, &bounds)) {  // if we're in the box, hilite... 
						gInIconBox = true;					
                    	    /* queue up an event */
                        WaitNextEvent(0, &theEvent,0,null);
                    	fn = interpreterProxy->ioLoadFunctionFrom("recordDragDropEvent", "");
                    	if (fn != 0) {
                    	    ((int (*) (EventRecord *theEvent, int numberOfItems, int dragType)) fn)(&theEvent, gNumDropFiles,DragEnter);
                    	}
					} 
				}
			}
			break;

		case kDragTrackingInWindow:
			if (gApprovedDrag) {
                WaitNextEvent(0, &theEvent,0,null);
            	fn = interpreterProxy->ioLoadFunctionFrom("recordDragDropEvent", "");
            	if (fn != 0) {
            	    ((int (*) (EventRecord *theEvent,  int numberOfItems, int dragType)) fn)(&theEvent,gNumDropFiles,DragMove);
            	    
            	}
			}
			break;

		case kDragTrackingLeaveWindow:
			if (gApprovedDrag && gInIconBox) {
            	    /* queue up an event */
                WaitNextEvent(0, &theEvent,0,null);
            	fn = interpreterProxy->ioLoadFunctionFrom("recordDragDropEvent", "");
            	if (fn != 0) {
            	    ((int (*) (EventRecord *theEvent, int numberOfItems, int dragType)) fn)(&theEvent, gNumDropFiles,DragLeave);
            	    
            	}
			}
			gApprovedDrag = gInIconBox = false;
			break;
	}
	return noErr; // there's no point in confusing Drag Manager or its caller
}


/* MyDragReceiveHandler is the receive handler for the main window.  It is called
	when a file or folder (or a promised file or folder) is dropped into the drop
	box in the main window.  Here, if the drag reference has been approved in the
	track drag call, we handle three different cases:
	
	1. standard hfs flavors,
	
	2. promised flavors provided by find file, mmmm This may be a pre sherlock issue
	
	3. promised flavors provided by other applications.
	
     */
     
pascal OSErr MyDragReceiveHandler(WindowPtr theWindow, void *refcon, DragReference theDragRef) {

	ItemReference   theItem;
	PromiseHFSFlavor targetPromise;
	Size            theSize;
	OSErr           err;
	EventRecord		theEvent;
	long            i,countActualItems,fn;
	FInfo 			finderInfo;
	HFSFlavor		targetHFSFlavor;
	
		/* validate the drag.  Recall the receive handler will only be called after
		the tracking handler has received a kDragTrackingInWindow event.  As a result,
		the gApprovedDrag and gInIconBox will be defined when we arrive here.  Hence,
		there is no need to spend extra time validating the drag at this point. */
		
	if ( ! (gApprovedDrag && gInIconBox) )  
	    return userCanceledErr; 

	if (gNumDropFiles !=0 ) 
	    DisposePtr((char *) dropFiles);
	    
	if ((err = CountDragItems(theDragRef, &gNumDropFiles)) != noErr) 
	    return paramErr;
	
	dropFiles = (HFSFlavor *) NewPtr(sizeof(HFSFlavor)*gNumDropFiles);
	
	if (dropFiles == null) {
	    gNumDropFiles = 0;
	    return userCanceledErr;
	}
	
    WaitNextEvent(0, &theEvent,0,null);
    countActualItems = 0;
    		
    for(i=1;i<=gNumDropFiles;i++) {
		/* get the first item reference */
    	if ((err = GetDragItemReferenceNumber(theDragRef, i, &theItem)) != noErr) 
    	    continue;

    		/* try to get a  HFSFlavor*/
    	theSize = sizeof(HFSFlavor);
    	err = GetFlavorData(theDragRef, theItem, flavorTypeHFS, &targetHFSFlavor, &theSize, 0);
    	if (err == noErr) {
    		if (dropFiles[countActualItems].fileCreator == 'MACS' && (
    				dropFiles[countActualItems].fileType == 'fold' ||
    				dropFiles[countActualItems].fileType == 'disk')) 
    				continue;
    		dropFiles[countActualItems] = targetHFSFlavor;
    		countActualItems++;
    		continue;
    	} else if (err != badDragFlavorErr) 
    	        continue; 
    	
    		/* try to get a  promised HFSFlavor*/
    	theSize = sizeof(PromiseHFSFlavor);
    	err = GetFlavorData(theDragRef, theItem, flavorTypePromiseHFS, &targetPromise, &theSize, 0);
    	if (err != noErr) 
    		continue;
    	
    		/* check for a drop from find file */
    	if (targetPromise.promisedFlavor == kPromisedFlavorFindFile) {
    	
    			/* from find file, no need to set the file location... */
    		theSize = sizeof(FSSpec);
    		err = GetFlavorData(theDragRef, theItem, targetPromise.promisedFlavor, &dropFiles[countActualItems].fileSpec, &theSize, 0);
    		if (err != noErr) 
    			continue;
    		FSpGetFInfo(&dropFiles[countActualItems].fileSpec, &finderInfo);
	    		/* queue up an event */
	        dropFiles[countActualItems].fileType = finderInfo.fdType;
	        dropFiles[countActualItems].fileCreator = finderInfo.fdCreator;
	        dropFiles[countActualItems].fdFlags =  finderInfo.fdFlags;
    		countActualItems++;
    	} else {
    		err = badDragFlavorErr;
    		return err;
    	}
    }
    
	gNumDropFiles = countActualItems;
    if (gNumDropFiles == 0) {
    	DisposePtr((char *) dropFiles);
    	return noErr;
    }
	
	    /* queue up an event */

	fn = interpreterProxy->ioLoadFunctionFrom("recordDragDropEvent", "");
	if (fn != 0) {
	    ((int (*) (EventRecord *theEvent, int numberOfItems, int dragType)) fn)(&theEvent, gNumDropFiles,DragDrop);
	}
	return noErr;
}

void sqSetNumberOfDropFiles(int numberOfFiles) {
	if (gNumDropFiles != 0 ) {
	    DisposePtr((char *) dropFiles);
	    gNumDropFiles = 0;
	}
	gNumDropFiles = numberOfFiles;
	dropFiles = (HFSFlavor *) NewPtr(sizeof(HFSFlavor)*gNumDropFiles);
	if (dropFiles == null) {
	    gNumDropFiles = 0;
	}
    return;
}

void sqSetFileInformation(int dropIndex, void *dropFile) { 
    if(dropIndex < 1 || dropIndex > gNumDropFiles) 
        return;
    memcpy(&dropFiles[dropIndex-1],(char *) dropFile,sizeof(HFSFlavor));
}
