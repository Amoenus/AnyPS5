# libSceOpusDec technical debt

### Additional callable exports

- [Additional callable exports](../../../core/libs/prx/libSceOpusDec/Export.cpp) - One entry observed in the 3.20 export inventory and present in the later 13.60-labelled inventory are import-resolution declarations only. The later inventory label does not authenticate a retail firmware version. Parameter lists, return types and behavior are unverified; parameterless `int` declarations are placeholders, not verified ABI contracts. Every entry throws through `NotImplemented_nid_no_patch`. No runtime functionality is implemented. Identified candidate names match their NIDs in the [public symbol database](https://github.com/zecoxao/sce_symbols/blob/2883963a0a514ba6e77407a08ef4730a65e88254/aerolib.csv) or [supplemental catalog](https://github.com/claimore22/ps5rs/blob/2304be5131221bae0dac24b9767cabffda33f747/data/nids.csv).

| Name | NID |
| --- | --- |
| `sceOpusDecGetAjmContext` | `OyDFNZOLrp0` |


### Unknown function info

- [sceOpusDecCancel, sceOpusDecCreate and sceOpusDecCtl](../../../core/libs/prx/libSceOpusDec/Export.cpp) (libSceOpusDec) - names and NIDs are present in the [PS5 3.20 export inventory](https://github.com/DNNDHH/PS5-3.20_Libs/blob/b2a77c09cd71c01f34c790cfbe98c751a13262ad/libSceOpusDec.c), but no credible prototypes were found. They are declared without parameters solely to resolve known imports; this is not an ABI claim.
- [sceOpusDecDecodeWithPriorityAndTimeout and sceOpusDecDecodeFloatWithPriorityAndTimeout](../../../core/libs/prx/libSceOpusDec/Export.cpp) (libSceOpusDec) decode synchronously only for the infinite timeout 0xFFFFFFFF, the value sceOpusDecDecode passes, so the batch ID output is never written. libSceOpusDec.sprx (firmware 13.60, read from disassembly, not measured on a console) passes the timeout to its AJM batch wait; when the wait does not finish it stores the batch ID and returns 0x81740015. Whether a finite timeout finishes depends on the console's decode time, so any other timeout returns the argument errors and then throws. The priority (20 to 100) is validated but does not schedule anything

### Functional

- [libSceOpusDec](../../../core/libs/prx/libSceOpusDec/Export.cpp) uses bundled libopus 1.6.1 for mono/stereo signed-16 decoding at the creation rates the research lists: 8000, 12000, 16000, 24000 and 48000 Hz. The supplied module confirms the exported arguments, read-only context inputs, 640-byte state size and creation rates. Validation errors and their precedence are recovered. Console PCM equivalence, AJM failures, context ownership and termination with live decoders remain unimplemented. Packet loss concealment still throws; decoder state is kept in host registries instead of the native guest state layout.
