# libSceWebBrowserDialog technical debt

### Silent stubs

- [libSceWebBrowserDialog](../../../core/libs/prx/libSceWebBrowserDialog/Export.cpp) - `sceWebBrowserDialogOpen` and `sceWebBrowserDialogOpenForPredeterminedContent` run the common-dialog state machine without displaying an interactive browser, and `sceWebBrowserDialogGetResult` reports that the user cancelled. Minecraft Bedrock (PPSA17221) imports `sceWebBrowserDialogOpenForPredeterminedContent` (#5744). The 424-byte allow-list parameter and its twenty optional address pointers are validated, but address filtering is not modelled without an interactive browser. Main open-parameter validation remains incomplete.
- [sceWebBrowserDialogSetCookie, sceWebBrowserDialogResetCookie](../../../core/libs/prx/libSceWebBrowserDialog/Export.cpp) (libSceWebBrowserDialog) - there is no browser cookie store. Both calls check their parameters in the same order and with the same codes as the PS5 13.60 module, then return 0 without storing or clearing anything. On the console they then forward the request to the browser service. While an opened dialog has not been updated to finished, closed or terminated, they return `0x80B80006` (the console's open-dialog flag).
### Unknown function info

- [sceWebBrowserDialogGetEvent, sceWebBrowserDialogNavigate, sceWebBrowserDialogSetZoom](../../../core/libs/prx/libSceWebBrowserDialog/Export.cpp) (libSceWebBrowserDialog) - their names and NIDs are in the [PS5 3.20 export inventory](https://github.com/DNNDHH/PS5-3.20_Libs/blob/b2a77c09cd71c01f34c790cfbe98c751a13262ad/libSceWebBrowserDialog.c); the no-parameter prototypes follow [shadPS4 `webbrowserdialog.cpp`](https://github.com/shadps4-emu/shadPS4/blob/f68923fe807f657348e6719ce658bcbd0a2b1c72/src/core/libraries/web_browser_dialog/webbrowserdialog.cpp#L23-L85) and are not measured on a console. They are declared without parameters solely to resolve known imports; this is not an ABI claim, and every call throws.

### Additional callable exports

- [Additional callable exports](../../../core/libs/prx/libSceWebBrowserDialog/Export.cpp) - 1 entries observed in the 3.20 export inventory and present in the later 13.60-labelled inventory are import-resolution declarations only. The later inventory label does not authenticate a retail firmware version. Parameter lists, return types and behavior are unverified; parameterless `int` declarations are placeholders, not verified ABI contracts. Every entry throws through `NotImplemented_nid_no_patch`. Unidentified names use literal NID aliases. No runtime functionality is implemented. Data exports require separate layout and ownership analysis. Identified candidate names match their NIDs in the [public symbol database](https://github.com/zecoxao/sce_symbols/blob/2883963a0a514ba6e77407a08ef4730a65e88254/aerolib.csv) or [supplemental catalog](https://github.com/claimore22/ps5rs/blob/2304be5131221bae0dac24b9767cabffda33f747/data/nids.csv).

| Name | NID |
| --- | --- |
| `Unknown` | `8r4EJ3FiX4w` |
