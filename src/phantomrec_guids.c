// phantomrec_guids.c — PhantomRec GUID definitions
// "Every screen deserves to be recorded."
// Built by MaxRBLX1
//
// This translation unit exists solely to emit the WASAPI / MMDevice COM
// GUID symbols that PhantomRec's core references. Doing it here, in
// isolation, guarantees INITGUID is in effect before any Windows header
// touches guiddef.h — which is the only reliable way to get these symbols
// defined exactly once.

#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>

DEFINE_GUID(KSDATAFORMAT_SUBTYPE_IEEE_FLOAT,
    0x00000003, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
