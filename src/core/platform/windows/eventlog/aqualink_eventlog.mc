;// Aqualink Automate - Windows Event Log message table.
;//
;// The runtime Event Log sink is Boost.Log's simple_event_log_backend
;// (src/core/platform/windows/native_log_sink.cpp), which reports events with these
;// fixed identifiers and a single insertion string (the formatted log line). Defining
;// the same identifiers here -- each body being just "%1" -- makes Event Viewer render
;// our text verbatim, with no "The description for Event ID ... cannot be found"
;// wrapper. This table is compiled into the executable (see src/CMakeLists.txt) and
;// pointed at by EventMessageFile (RegisterLogSource writes EventMessageFile = the exe).
;//
;// Facility is 0 and the severity is encoded in the top two bits, so the composed
;// 32-bit event IDs are exactly the values the backend emits:
;//   Debug/Success 0x00000100, Info 0x40000101, Warning 0x80000102, Error 0xC0000103.

MessageIdTypedef=DWORD

SeverityNames=(Success=0x0:STATUS_SEVERITY_SUCCESS
               Informational=0x1:STATUS_SEVERITY_INFORMATIONAL
               Warning=0x2:STATUS_SEVERITY_WARNING
               Error=0x3:STATUS_SEVERITY_ERROR
              )

FacilityNames=(Application=0x0:FACILITY_APPLICATION)

LanguageNames=(English=0x409:MSG00409)

MessageId=0x100
Severity=Success
Facility=Application
SymbolicName=AQUALINK_MSG_DEBUG
Language=English
%1
.

MessageId=0x101
Severity=Informational
Facility=Application
SymbolicName=AQUALINK_MSG_INFO
Language=English
%1
.

MessageId=0x102
Severity=Warning
Facility=Application
SymbolicName=AQUALINK_MSG_WARNING
Language=English
%1
.

MessageId=0x103
Severity=Error
Facility=Application
SymbolicName=AQUALINK_MSG_ERROR
Language=English
%1
.
