//  Copyright © 2023 ChefKiss Inc. Licensed under the Thou Shalt Not Profit License version 1.0. See LICENSE for
//  details.
#include "kern_gen11.hpp"
#include <Headers/kern_api.hpp>
#include "kern_genx.hpp"
#include "kern_green.hpp"
#include <IOKit/IOCatalogue.h>

// ==== 4 kextInfos: LE priority (path[0]), SLE fallback (path[1]) ====

// ICL FB — com.apple (in kernel collection)
static const char *pathsICLFB[] = {
    "/Library/Extensions/AppleIntelICLLPGraphicsFramebuffer.kext/Contents/MacOS/AppleIntelICLLPGraphicsFramebuffer",
    "/System/Library/Extensions/AppleIntelICLLPGraphicsFramebuffer.kext/Contents/MacOS/AppleIntelICLLPGraphicsFramebuffer",
};
static KernelPatcher::KextInfo kextG11FB {"com.apple.driver.AppleIntelICLLPGraphicsFramebuffer", pathsICLFB, 2, {}, {},
    KernelPatcher::KextInfo::Unloaded};

// ICL HW — com.apple (in kernel collection)
static const char *pathsICLHW[] = {
    "/Library/Extensions/AppleIntelICLGraphics.kext/Contents/MacOS/AppleIntelICLGraphics",
    "/System/Library/Extensions/AppleIntelICLGraphics.kext/Contents/MacOS/AppleIntelICLGraphics",
};
static KernelPatcher::KextInfo kextG11HW {"com.apple.driver.AppleIntelICLGraphics", pathsICLHW, 2, {}, {},
    KernelPatcher::KextInfo::Unloaded};

// TGL FB — com.xxxxx (loaded from /Library/Extensions/)
static const char *pathsTGLFB[] = {
    "/Library/Extensions/AppleIntelTGLGraphicsFramebuffer.kext/Contents/MacOS/AppleIntelTGLGraphicsFramebuffer",
    "/System/Library/Extensions/AppleIntelTGLGraphicsFramebuffer.kext/Contents/MacOS/AppleIntelTGLGraphicsFramebuffer",
};
static KernelPatcher::KextInfo kextG11FBT {"com.xxxxx.driver.AppleIntelTGLGraphicsFramebuffer", pathsTGLFB, 2,
    {false, false, false, true}, {},
    KernelPatcher::KextInfo::Unloaded};

// TGL HW — com.xxxxx (loaded from /Library/Extensions/)
static const char *pathsTGLHW[] = {
    "/Library/Extensions/AppleIntelTGLGraphics.kext/Contents/MacOS/AppleIntelTGLGraphics",
    "/System/Library/Extensions/AppleIntelTGLGraphics.kext/Contents/MacOS/AppleIntelTGLGraphics",
};
static KernelPatcher::KextInfo kextG11HWT {"com.xxxxx.driver.AppleIntelTGLGraphics", pathsTGLHW, 2,
    {false, false, false, true}, {},
    KernelPatcher::KextInfo::Unloaded};

Gen11 *Gen11::callback = nullptr;

void Gen11::init() {
	callback = this;
	// 4 kextInfos: ICL FB, ICL HW, TGL FB, TGL HW
	lilu.onKextLoadForce(&kextG11FB);
	lilu.onKextLoadForce(&kextG11HW);
	lilu.onKextLoadForce(&kextG11FBT);
	lilu.onKextLoadForce(&kextG11HWT);
}

static bool isWEGCoexistMode() {
	int enabled = 0;
	if (PE_parse_boot_argn("nbwegcoex", &enabled, sizeof(enabled))) {
		return enabled != 0;
	}

	return checkKernelArgument("-nbwegcoex");
}

bool Gen11::processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	
	if (kextG11FB.loadIndex == index) {
		if (this->tglFBLoaded) {
			DBGLOG("ngreen", "Skipping ICL FB — TGL FB already loaded");
			return true;
		}
		auto *activeKext = &kextG11FB;
		DBGLOG("ngreen", "init AppleIntelICLLPGraphicsFramebuffer!");
		//NGreen::callback->igfxGen = iGFXGen::Gen11;
		NGreen::callback->setRMMIOIfNecessary();
		
		const bool wegCoexist = isWEGCoexistMode();
		if (wegCoexist) {
			SYSLOG("nblue", "WEG coexist mode enabled: skipping NootedBlue CDCLK route overlap");
		}

		if (wegCoexist) {
			SolveRequestPlus solveRequests[] = {
			//		{"__ZN31AppleIntelFramebufferController14disableCDClockEv", this->orgDisableCDClock},
			//		{"__ZN31AppleIntelFramebufferController19setCDClockFrequencyEy", this->orgSetCDClockFrequency},
			//		{"__ZN20IntelFBClientControl11doAttributeEjPmmS0_S0_P25IOExternalMethodArguments", this->orgFBClientDoAttribute},
			//		{"__ZN31AppleIntelFramebufferController5startEP9IOService",	this->ostart},
			//		{"__ZN31AppleIntelFramebufferController14ReadRegister32Em",	this->oreadRegister32},
		 		{"__ZN31AppleIntelFramebufferController20hwConfigureCustomAUXEb", this->ohwConfigureCustomAUX},
			};
			PANIC_COND(!SolveRequestPlus::solveAll(patcher, index, solveRequests, address, size), "nblue",	"Failed to resolve symbols");
		} else {
			SolveRequestPlus solveRequests[] = {
			//		{"__ZN31AppleIntelFramebufferController14disableCDClockEv", this->orgDisableCDClock},
			//		{"__ZN31AppleIntelFramebufferController19setCDClockFrequencyEy", this->orgSetCDClockFrequency},
			//		{"__ZN20IntelFBClientControl11doAttributeEjPmmS0_S0_P25IOExternalMethodArguments", this->orgFBClientDoAttribute},
			//		{"__ZN31AppleIntelFramebufferController5startEP9IOService",	this->ostart},
			//		{"__ZN31AppleIntelFramebufferController14ReadRegister32Em",	this->oreadRegister32},
		 		{"__ZN31AppleIntelFramebufferController20hwConfigureCustomAUXEb", this->ohwConfigureCustomAUX},
				{"__ZN31AppleIntelFramebufferController21probeCDClockFrequencyEv", this->orgProbeCDClockFrequency},
			};
			PANIC_COND(!SolveRequestPlus::solveAll(patcher, index, solveRequests, address, size), "nblue",	"Failed to resolve symbols");
		}
		
		if (wegCoexist) {
			RouteRequestPlus requests[] = {
				// Keep stock ReadRegister32 while stabilizing display pipeline behavior.
			//		{"__ZN31AppleIntelFramebufferController14ReadRegister32Em",wrapReadRegister32,	this->owrapReadRegister32},
			//		{"__ZN21AppleIntelFramebuffer13SaveNVRAMModeEv",handleLinkIntegrityCheck},
				// Keep stock wake/sleep lifecycle handlers to avoid broken restore paths.
				//{"__ZN21AppleIntelFramebuffer18prepareToEnterWakeEv",dovoid},
				//{"__ZN21AppleIntelFramebuffer17prepareToExitWakeEv",dovoid},
				//{"__ZN21AppleIntelFramebuffer18prepareToExitSleepEv",dovoid},
				//{"__ZN21AppleIntelFramebuffer19prepareToEnterSleepEv",dovoid},
				// Keep stock doAttribute while chasing UI stalls / high WindowServer CPU.
			//		{"__ZN20IntelFBClientControl11doAttributeEjPmmS0_S0_P25IOExternalMethodArguments",wrapFBClientDoAttribute,	this->orgFBClientDoAttribute},
				//ADDED
			//{"__ZN21AppleIntelFramebuffer4initEP31AppleIntelFramebufferControllerj",AppleIntelFramebufferinit, this->oAppleIntelFramebufferinit},
			//		{"__ZN31AppleIntelFramebufferController10hwShutdownEP21AppleIntelFramebuffer",handleLinkIntegrityCheck},
					{"__ZN31AppleIntelFramebufferController18hwInitializeCStateEv",hwInitializeCState, this->ohwInitializeCState},
			//		{"__ZN31AppleIntelFramebufferController20hwConfigureCustomAUXEb",hwConfigureCustomAUX, this->ohwConfigureCustomAUX},
			//	{"__ZN31AppleIntelFramebufferController21probeCDClockFrequencyEv",wrapProbeCDClockFrequency,	this->orgProbeCDClockFrequency},
			};
			PANIC_COND(!RouteRequestPlus::routeAll(patcher, index, requests, address, size), "nblue","Failed to route symbols");
		} else {
			RouteRequestPlus requests[] = {
				// Keep stock ReadRegister32 while stabilizing display pipeline behavior.
			//		{"__ZN31AppleIntelFramebufferController14ReadRegister32Em",wrapReadRegister32,	this->owrapReadRegister32},
			//		{"__ZN21AppleIntelFramebuffer13SaveNVRAMModeEv",handleLinkIntegrityCheck},
				// Keep stock wake/sleep lifecycle handlers to avoid broken restore paths.
				//{"__ZN21AppleIntelFramebuffer18prepareToEnterWakeEv",dovoid},
				//{"__ZN21AppleIntelFramebuffer17prepareToExitWakeEv",dovoid},
				//{"__ZN21AppleIntelFramebuffer18prepareToExitSleepEv",dovoid},
				//{"__ZN21AppleIntelFramebuffer19prepareToEnterSleepEv",dovoid},
				// Keep stock doAttribute while chasing UI stalls / high WindowServer CPU.
			//		{"__ZN20IntelFBClientControl11doAttributeEjPmmS0_S0_P25IOExternalMethodArguments",wrapFBClientDoAttribute,	this->orgFBClientDoAttribute},
				//ADDED
			//{"__ZN21AppleIntelFramebuffer4initEP31AppleIntelFramebufferControllerj",AppleIntelFramebufferinit, this->oAppleIntelFramebufferinit},
			//		{"__ZN31AppleIntelFramebufferController10hwShutdownEP21AppleIntelFramebuffer",handleLinkIntegrityCheck},
					{"__ZN31AppleIntelFramebufferController18hwInitializeCStateEv",hwInitializeCState, this->ohwInitializeCState},
			//		{"__ZN31AppleIntelFramebufferController20hwConfigureCustomAUXEb",hwConfigureCustomAUX, this->ohwConfigureCustomAUX},
			//	{"__ZN31AppleIntelFramebufferController21probeCDClockFrequencyEv",wrapProbeCDClockFrequency,	this->orgProbeCDClockFrequency},
				{"__ZN31AppleIntelFramebufferController11initCDClockEv",initCDClock,this->oinitCDClock}
			};
			PANIC_COND(!RouteRequestPlus::routeAll(patcher, index, requests, address, size), "nblue","Failed to route symbols");
		}
		//static const uint8_t f15[]= {0x00,0x02, 0x00, 0x5c, 0x8a};
		//static const uint8_t r15[]= {0x00,0x00, 0x00, 0x49, 0x9a};
		
		
		// AppleIntelFramebufferController::hwSetMode skip hwRegsNeedUpdate
		static const uint8_t f2[] = {0xE8, 0x31, 0xE5, 0xFF, 0xFF, 0x84, 0xC0, 0x74, 0x3D};
		static const uint8_t r2[] = {0xE8, 0x31, 0xE5, 0xFF, 0xFF, 0x84, 0xC0, 0xEB, 0x3D};
		
		//sonoma
		static const uint8_t f2b[] = {0xE8, 0x54, 0xEA, 0xFF, 0xFF, 0x84, 0xC0, 0x74, 0x5C};
		static const uint8_t r2b[] = {0xE8, 0x54, 0xEA, 0xFF, 0xFF, 0x84, 0xC0, 0xeb, 0x5C};
		
		//sequoia
		static const uint8_t f2c[] = {0xE8, 0x74, 0xEA, 0xFF, 0xFF, 0x84, 0xC0, 0x74, 0x5C};
		static const uint8_t r2c[] = {0xE8, 0x74, 0xEA, 0xFF, 0xFF, 0x84, 0xC0, 0xeb, 0x5C};
		
		/*if (getKernelVersion() <= KernelVersion::Ventura) {
			KernelPatcher::LookupPatch patch { &kextG11FB, f2, r2, sizeof(f2), 1 };
			patcher.applyLookupPatch(&patch);
		}
		
		if (getKernelVersion() == KernelVersion::Sonoma) {
			KernelPatcher::LookupPatch patchb { &kextG11FB, f2b, r2b, sizeof(f2b), 1 };
			patcher.applyLookupPatch(&patchb);
		}
		
		if (getKernelVersion() >= KernelVersion::Sequoia) {
			KernelPatcher::LookupPatch patchc { &kextG11FB, f2c, r2c, sizeof(f2c), 1 };
			patcher.applyLookupPatch(&patchc);
		}*/
		
		

		// Variant-consistent remap for constructor entries:
			// B8 xx 00 5C 8A -> B8 xx 00 49 9A and exact C7 05 ... 02 00 5C 8A site.
			static const uint8_t kPatchPlatformRemapMovEaxFind0[] = {0xB8, 0x00, 0x00, 0x5C, 0x8A};
			static const uint8_t kPatchPlatformRemapMovEaxReplace0[] = {0xB8, 0x00, 0x00, 0x49, 0x9A};
			static const uint8_t kPatchPlatformRemapMovEaxFind1[] = {0xB8, 0x01, 0x00, 0x5C, 0x8A};
			static const uint8_t kPatchPlatformRemapMovEaxReplace1[] = {0xB8, 0x01, 0x00, 0x49, 0x9A};
			static const uint8_t kPatchPlatformRemapMovEaxFind2[] = {0xB8, 0x02, 0x00, 0x5C, 0x8A};
			static const uint8_t kPatchPlatformRemapMovEaxReplace2[] = {0xB8, 0x02, 0x00, 0x49, 0x9A};
			static const uint8_t kPatchPlatformRemapC705Find2[] = {0xC7, 0x05, 0xE9, 0x9B, 0x05, 0x00, 0x02, 0x00, 0x5C, 0x8A};
			static const uint8_t kPatchPlatformRemapC705Replace2[] = {0xC7, 0x05, 0xE9, 0x9B, 0x05, 0x00, 0x02, 0x00, 0x49, 0x9A};

			// hwSetMode: bypass hwRegsNeedUpdate result (CALL hwRegsNeedUpdate; TEST AL,AL: JE+0x62 → JMP+0x62)
			// Verified unique (1 match at 0x94055) in ICL LP le binary. Forces register reprogram unconditionally. [ICL-LP]
			static const uint8_t kPatchHwRegsNeedUpdateBypassFind[] = {0xe8, 0xe2, 0xcc, 0xff, 0xff, 0x84, 0xc0, 0x74, 0x62};
			static const uint8_t kPatchHwRegsNeedUpdateBypassReplace[] = {0xe8, 0xe2, 0xcc, 0xff, 0xff, 0x84, 0xc0, 0xeb, 0x62};

			LookupPatchPlus const minPatches[] = {
				{&kextG11FB, kPatchPlatformRemapMovEaxFind0, kPatchPlatformRemapMovEaxReplace0, arrsize(kPatchPlatformRemapMovEaxFind0), 1},
				{&kextG11FB, kPatchPlatformRemapMovEaxFind1, kPatchPlatformRemapMovEaxReplace1, arrsize(kPatchPlatformRemapMovEaxFind1), 1},
				{&kextG11FB, kPatchPlatformRemapMovEaxFind2, kPatchPlatformRemapMovEaxReplace2, arrsize(kPatchPlatformRemapMovEaxFind2), 1},
				{&kextG11FB, kPatchPlatformRemapC705Find2, kPatchPlatformRemapC705Replace2, arrsize(kPatchPlatformRemapC705Find2), 1},
				{&kextG11FB, kPatchHwRegsNeedUpdateBypassFind, kPatchHwRegsNeedUpdateBypassReplace, arrsize(kPatchHwRegsNeedUpdateBypassFind), 1},  // hwSetMode always reprogram [ICL-LP]
			};
		
		PANIC_COND(!LookupPatchPlus::applyAll(patcher, minPatches , address, size), "ngreen", "kextG11FB Failed to apply patches!");
		//PANIC_COND
		
		DBGLOG("ngreen", "Loaded AppleIntelICLLPGraphicsFramebuffer!");
		return true;
		
		
	}	else if (kextG11FBT.loadIndex == index) {
		this->tglFBLoaded = true;
		auto *activeKext = &kextG11FBT;
		NGreen::callback->setRMMIOIfNecessary();
		SYSLOG("ngreen", "init AppleIntelTGLGraphicsFramebuffer");
		
		bool isprod=false;
		auto prod=patcher.solveSymbol(index, "__ZN24AppleIntelBaseController5startEP9IOService", address, size);
		if (!prod) isprod=true;
		
		if (isprod) {
			
			SolveRequestPlus solveRequests[] = {
				{"__ZN31AppleIntelFramebufferController19setCDClockFrequencyEy", this->orgSetCDClockFrequency},
				
			};
			PANIC_COND(!SolveRequestPlus::solveAll(patcher, index, solveRequests, address, size), "ngreen",	"Failed to resolve symbols");
		}
		else
		{
			SolveRequestPlus solveRequests[] = {
				{"__ZN24AppleIntelBaseController19setCDClockFrequencyEy", this->orgSetCDClockFrequency},
				
			};
			PANIC_COND(!SolveRequestPlus::solveAll(patcher, index, solveRequests, address, size), "ngreen",	"Failed to resolve symbols");
			
		}

		RouteRequestPlus requests[] = {
			// ...existing routes...
			{"__ZN24AppleIntelBaseController17registerWithAICPMEPv", alwaysReturnSuccess, this->oalwaysReturnSuccess},
			// ...existing routes...
			{"__ZN19AppleIntelPowerWell21hwSetPowerWellStatePGEbj.cold.1",releaseDoorbell},
			{"__ZN19AppleIntelPowerWell21hwSetPowerWellStatePGEbj.cold.2",releaseDoorbell},
			{"__ZN19AppleIntelPowerWell21hwSetPowerWellStatePGEbj.cold.3",releaseDoorbell},
			{"__ZN19AppleIntelPowerWell21hwSetPowerWellStatePGEbj.cold.4",releaseDoorbell},
			{"__ZN19AppleIntelPowerWell21hwSetPowerWellStatePGEbj.cold.5",releaseDoorbell},
			{"__ZN19AppleIntelPowerWell21hwSetPowerWellStatePGEbj.cold.6",releaseDoorbell},
			{"__ZN19AppleIntelPowerWell21hwSetPowerWellStatePGEbj.cold.7",releaseDoorbell},
			{"__ZN19AppleIntelPowerWell21hwSetPowerWellStatePGEbj.cold.8",releaseDoorbell},
			{"__ZN19AppleIntelPowerWell21hwSetPowerWellStatePGEbj.cold.9",releaseDoorbell},
			{"__ZN19AppleIntelPowerWell21hwSetPowerWellStatePGEbj.cold.10",releaseDoorbell},
			{"__ZN19AppleIntelPowerWell21hwSetPowerWellStatePGEbj.cold.11",releaseDoorbell},
			{"__ZN19AppleIntelPowerWell21hwSetPowerWellStatePGEbj.cold.12",releaseDoorbell},
			{"__ZN19AppleIntelPowerWell22hwSetPowerWellStateAuxEbj.cold.1",releaseDoorbell},
			{"__ZN16AppleIntelScaler13disableScalerEb",disableScaler, this->odisableScaler},
			{"__ZN15AppleIntelPlane11enablePlaneEb",enablePlane, this->oenablePlane},
			{"__ZN16AppleIntelScaler17programPipeScalerEP21AppleIntelDisplayPath",programPipeScaler, this->oprogramPipeScaler},
			{"__ZN15AppleIntelPlane19updateRegisterCacheEv",AppleIntelPlaneupdateRegisterCache, this->oAppleIntelPlaneupdateRegisterCache},
			{"__ZN16AppleIntelScaler19updateRegisterCacheEv",AppleIntelScalerupdateRegisterCache, this->oAppleIntelScalerupdateRegisterCache},
			{"__ZN19AppleIntelPowerWell20disableDisplayEngineEv",disableDisplayEngine, this->odisableDisplayEngine},
			{"__ZN19AppleIntelPowerWell19enableDisplayEngineEv",enableDisplayEngine, this->oenableDisplayEngine},
			{"__ZN17AppleIntelPortHAL14enableComboPhyEv",enableComboPhyEv, this->oenableComboPhyEv},
			{"__ZN14AppleIntelPort16computeLaneCountEPK29IODetailedTimingInformationV2jjPj",computeLaneCount, this->ocomputeLaneCount},
			{"__ZN19AppleIntelPowerWell21hwSetPowerWellStatePGEbj", releaseDoorbell},
			{"__ZN19AppleIntelPowerWell22hwSetPowerWellStateAuxEbj",hwSetPowerWellStateAux, this->ohwSetPowerWellStateAux},
			{"__ZN19AppleIntelPowerWell22hwSetPowerWellStateDDIEbj",hwSetPowerWellStateDDI, this->ohwSetPowerWellStateDDI},
			{"__ZN31AppleIntelRegisterAccessManager19FastWriteRegister32Emj",FastWriteRegister32, this->oFastWriteRegister32},
			/*{"__ZN31AppleIntelRegisterAccessManager14ReadRegister32Em",raReadRegister32, this->oraReadRegister32},
			{"__ZN31AppleIntelRegisterAccessManager14ReadRegister32EPVvm",raReadRegister32b},*/
			{"__ZN31AppleIntelRegisterAccessManager15WriteRegister32Emj",raWriteRegister32, this->oraWriteRegister32},
			{"__ZN31AppleIntelRegisterAccessManager15WriteRegister32EPVvmj",raWriteRegister32b},
			{"__ZN21AppleIntelFramebuffer17prepareToExitWakeEv",releaseDoorbell},
			{"__ZN21AppleIntelFramebuffer18prepareToEnterWakeEv",releaseDoorbell},
			{"__ZN21AppleIntelFramebuffer18prepareToExitSleepEv",releaseDoorbell},
			{"__ZN21AppleIntelFramebuffer19prepareToEnterSleepEv",releaseDoorbell},
			//******
			{"__ZN24AppleIntelBaseController15enableVDDForAuxEP14AppleIntelPort", releaseDoorbell},
			// Keep native SST timing setup; forcing custom clocks can break CoreDisplay validation.
			//{"__ZN24AppleIntelBaseController17SetupDPSSTTimingsEP21AppleIntelFramebufferP21AppleIntelDisplayPathP10CRTCParams", SetupDPSSTTimings, this->oSetupDPSSTTimings},
			//{"__ZN24AppleIntelBaseController12SetupTimingsEP21AppleIntelFramebufferP21AppleIntelDisplayPathPK29IODetailedTimingInformationV2P10CRTCParams", SetupTimings, this->oSetupTimings},
			// Keep native detailed timing validation; avoid overriding pixel clock fields.
			//{"__ZN21AppleIntelFramebuffer22validateDetailedTimingEPvy", validateDetailedTiming, this->ovalidateDetailedTiming},
			//{"__ZN21AppleIntelFramebuffer19validateDisplayModeEiPPKNS_15ModeDescriptionEPPK29IODetailedTimingInformationV2", validateDisplayMode, this->ovalidateDisplayMode},
	   //     {"__ZN21AppleIntelFramebuffer18setupDisplayTimingEPK29IODetailedTimingInformationV2PS0_", setupDisplayTiming, this->osetupDisplayTiming},
			//{"__ZN21AppleIntelFramebuffer18maxSupportedDepthsEPK29IODetailedTimingInformationV2", maxSupportedDepths, this->omaxSupportedDepths},
			//{"__ZN21AppleIntelFramebuffer17validateModeDepthEPK29IODetailedTimingInformationV2j", validateModeDepth, this->ovalidateModeDepth},
			//*****
			//{"__ZN21AppleIntelFramebuffer19getPixelInformationEiiiP18IOPixelInformation", getPixelInformation, this->ogetPixelInformation},
			//{"__ZN20IntelFBClientControl11doAttributeEjPmmS0_S0_P25IOExternalMethodArguments",wrapFBClientDoAttribute, this->orgFBClientDoAttribute},
			//{"__ZN20IntelFBClientControl24vendor_doDeviceAttributeEjPmmS0_S0_P25IOExternalMethodArguments", releaseDoorbell},
			//{"__ZN21AppleIntelFramebuffer16enableControllerEv", isPanelPowerOn},
		};
		PANIC_COND(!RouteRequestPlus::routeAll(patcher, index, requests, address, size), "ngreen","Failed to route dp symbols");
		
		if (isprod) {
			RouteRequestPlus requests[] = {
				{"__ZN21AppleIntelFramebuffer4initEP31AppleIntelFramebufferControllerj",AppleIntelFramebufferinit, this->oAppleIntelFramebufferinit},
				{"__ZN31AppleIntelFramebufferController10hwShutdownEP21AppleIntelFramebuffer",handleLinkIntegrityCheck},
				{"__ZN31AppleIntelFramebufferController18hwInitializeCStateEv",hwInitializeCState, this->ohwInitializeCState},
				{"__ZN31AppleIntelFramebufferController20hwConfigureCustomAUXEb",hwConfigureCustomAUX, this->ohwConfigureCustomAUX},
				{"__ZN19AppleIntelPowerWell4initEP31AppleIntelFramebufferController",AppleIntelPowerWellinit, this->oAppleIntelPowerWellinit},
				{"__ZN31AppleIntelFramebufferController5startEP9IOService",AppleIntelBaseControllerstart, this->oAppleIntelBaseControllerstart},
				{"__ZN31AppleIntelFramebufferController21probeCDClockFrequencyEv",wrapProbeCDClockFrequency,	this->orgProbeCDClockFrequency},
				{"__ZN31AppleIntelFramebufferController11initCDClockEv",initCDClock, this->oinitCDClock},
				{"__ZN31AppleIntelFramebufferController28setCDClockFrequencyOnHotplugEv",setCDClockFrequencyOnHotplug, this->osetCDClockFrequencyOnHotplug},
				{"__ZN31AppleIntelFramebufferController14disableCDClockEv",disableCDClock,this->odisableCDClock},
				{"__ZN31AppleIntelFramebufferController16hwRegsNeedUpdateEP21AppleIntelFramebufferP21AppleIntelDisplayPathP10CRTCParamsPK29IODetailedTimingInformationV2PN16AppleIntelScaler12SCALERPARAMSE",hwRegsNeedUpdate, this->ohwRegsNeedUpdate},
			};
			PANIC_COND(!RouteRequestPlus::routeAll(patcher, index, requests, address, size), "ngreen","Failed to route p symbols");
			
		} else
		{
			RouteRequestPlus requests[] = {
				{"__ZN21AppleIntelFramebuffer4initEP24AppleIntelBaseControllerj",AppleIntelFramebufferinit, this->oAppleIntelFramebufferinit},
				{"__ZN24AppleIntelBaseController10hwShutdownEP21AppleIntelFramebuffer",handleLinkIntegrityCheck},
				{"__ZN24AppleIntelBaseController18hwInitializeCStateEv",hwInitializeCState, this->ohwInitializeCState},
				{"__ZN24AppleIntelBaseController20hwConfigureCustomAUXEb",hwConfigureCustomAUX, this->ohwConfigureCustomAUX},
				{"__ZN19AppleIntelPowerWell4initEP24AppleIntelBaseController",AppleIntelPowerWellinit, this->oAppleIntelPowerWellinit},
				{"__ZN24AppleIntelBaseController5startEP9IOService",AppleIntelBaseControllerstart, this->oAppleIntelBaseControllerstart},
				{"__ZN24AppleIntelBaseController21probeCDClockFrequencyEv",wrapProbeCDClockFrequency,	this->orgProbeCDClockFrequency},
				{"__ZN24AppleIntelBaseController11initCDClockEv",initCDClock, this->oinitCDClock},
				{"__ZN24AppleIntelBaseController28setCDClockFrequencyOnHotplugEv",setCDClockFrequencyOnHotplug, this->osetCDClockFrequencyOnHotplug},
				{"__ZN24AppleIntelBaseController14disableCDClockEv",disableCDClock,this->odisableCDClock},
				{"__ZN24AppleIntelBaseController16hwRegsNeedUpdateEP21AppleIntelFramebufferP21AppleIntelDisplayPathP10CRTCParamsPK29IODetailedTimingInformationV2PN16AppleIntelScaler12SCALERPARAMSE",hwRegsNeedUpdate, this->ohwRegsNeedUpdate},
				
			};
			PANIC_COND(!RouteRequestPlus::routeAll(patcher, index, requests, address, size), "ngreen","Failed to route d symbols");
			
		}
		
		//powerwell
		static const uint8_t f1[]= {0xe8, 0x99, 0x9f, 0xfd, 0xff, 0x89, 0x45, 0xc8, 0x3d, 0xff, 0xff, 0x00, 0x00, 0x74, 0x78};
		static const uint8_t r1[]= {0xe8, 0x99, 0x9f, 0xfd, 0xff, 0x89, 0x45, 0xc8, 0x3d, 0xff, 0xff, 0x00, 0x00, 0xeb, 0x78};
		
		static const uint8_t f1p[]= {0xe8, 0x66, 0xb0, 0xfe, 0xff, 0x89, 0x45, 0xc8, 0x3d, 0xff, 0xff, 0x00, 0x00, 0x74, 0x45};
		static const uint8_t r1p[]= {0xe8, 0x66, 0xb0, 0xfe, 0xff, 0x89, 0x45, 0xc8, 0x3d, 0xff, 0xff, 0x00, 0x00, 0xeb, 0x45};
		
		//osinfo
		/*fInfoHasLid                  : 1
		fInfoPipeCount               : 3
		fInfoPortCount               : 3
		fInfoFramebufferCount        : 3*/

		static const uint8_t f2[]= {0xc7, 0x05, 0x07, 0x81, 0x10, 0x00, 0x01, 0x03, 0x09, 0x03, 0xb8, 0x00, 0x00, 0x00, 0x04};
		static const uint8_t r2[]= {0xc7, 0x05, 0x07, 0x81, 0x10, 0x00, 0x01, 0x04, 0x03, 0x02, 0xb8, 0x00, 0x00, 0x00, 0x04};

		static const uint8_t f2p[]= {0xc7, 0x05, 0x57, 0xe5, 0x0b, 0x00, 0x01, 0x03, 0x09, 0x03};
		static const uint8_t r2p[]= {0xc7, 0x05, 0x57, 0xe5, 0x0b, 0x00, 0x01, 0x04, 0x03, 0x02};
		
		static const uint8_t f2b[]= {0x49, 0xbe, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x4c, 0x89, 0x35, 0x17, 0x81, 0x10, 0x00, 0xb8, 0x08, 0x00, 0x00, 0x00};
		static const uint8_t r2b[]= {0x49, 0xbe, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x4c, 0x89, 0x35, 0x17, 0x81, 0x10, 0x00, 0xb8, 0x08, 0x00, 0x00, 0x00};
		
		static const uint8_t f2c[]= {0x48, 0x89, 0x1d, 0xc0, 0x81, 0x10, 0x00, 0x4c, 0x89, 0x35, 0xc1, 0x81, 0x10, 0x00, 0x48, 0xb8, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00};
		static const uint8_t r2c[]= {0x48, 0x89, 0x1d, 0xc0, 0x81, 0x10, 0x00, 0x4c, 0x89, 0x35, 0xc1, 0x81, 0x10, 0x00, 0x48, 0xb8, 0x05, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00};
		
		//mem
		static const uint8_t f2d[]= {0x0f, 0x94, 0xc0, 0xb9, 0x00, 0x00, 0x10, 0x00, 0xba, 0x00, 0x00, 0x80, 0x00};
		static const uint8_t r2d[]= {0x0f, 0x94, 0xc0, 0xb9, 0x00, 0x00, 0x10, 0x00, 0xba, 0x00, 0x00, 0x40, 0x00};
		
		//mem
		static const uint8_t f2dp[]= {0xb8, 0x00, 0x00, 0x10, 0x00, 0xba, 0x00, 0x00, 0x80, 0x00, 0x0f, 0x44, 0xd0, 0x0f, 0x94, 0xc1, 0x48, 0x01, 0x0d, 0xa2, 0xd0, 0x09, 0x00};
		static const uint8_t r2dp[]= {0xb8, 0x00, 0x00, 0x10, 0x00, 0xba, 0x00, 0x00, 0x40, 0x00, 0x0f, 0x44, 0xd0, 0x0f, 0x94, 0xc1, 0x48, 0x01, 0x0d, 0xa2, 0xd0, 0x09, 0x00};
		
		//cdclock
		static const uint8_t f2e[]= {0x48, 0xc7, 0x83, 0x60, 0x43, 0x00, 0x00, 0x00, 0x2d, 0x31, 0x01, 0x48, 0xc7, 0x83, 0x68, 0x43, 0x00, 0x00, 0x00, 0x54, 0xea, 0x2a, 0xc6, 0x83, 0xb4, 0x45, 0x00, 0x00, 0x00};
		static const uint8_t r2e[]= {0x48, 0xc7, 0x83, 0x60, 0x4a, 0x00, 0x00, 0x00, 0xa3, 0x02, 0x00, 0x48, 0xc7, 0x83, 0x68, 0x4a, 0x00, 0x00, 0x00, 0xf6, 0x09, 0x00, 0xc6, 0x83, 0xb4, 0x45, 0x00, 0x00, 0x00};
		


		//conn
		static const uint8_t f3[]= {
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00,
			0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
			0x02, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
			0x03, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
			0x04, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
			0x05, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
			0x06, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
			0x07, 0x00, 0x00, 0x00, 0x0d, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
			0x08, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00};
		
		static const uint8_t r3[]= {
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00,
			0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
			0x02, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
		
		
		//lcd power reg
		static const uint8_t f4[]= {0x00, 0x72, 0x0c, 0x00};
		static const uint8_t r4[]= {0x00, 0x12, 0x06, 0x00};
		
		static const uint8_t f4a[]= {0x04, 0x72, 0x0c, 0x00};
		static const uint8_t r4a[]= {0x04, 0x12, 0x06, 0x00};
		
		static const uint8_t f4b[]= {0x08, 0x72, 0x0c, 0x00};
		static const uint8_t r4b[]= {0x08, 0x12, 0x06, 0x00};
		
		static const uint8_t f4c[]= {0x0c, 0x72, 0x0c, 0x00};
		static const uint8_t r4c[]= {0x0c, 0x12, 0x06, 0x00};
		
		
		//jalavoui
		static const uint8_t f6a[]= { 0xbe, 0x04, 0x00, 0x00, 0x00, 0x48, 0x89, 0xda, 0x31, 0xc9, 0xe8, 0x8c, 0xac, 0x04, 0x00};
		static const uint8_t r6a[]= { 0xbe, 0x04, 0x00, 0x00, 0x00, 0x48, 0x89, 0xda, 0x31, 0xc9, 0x90, 0x90, 0x90, 0x90, 0x90};
		
		//ReadRegister64
		static const uint8_t f7[]= {0x83, 0xc0, 0xfc, 0x48, 0x39, 0xf0, 0x76, 0x11, 0x48, 0x8b, 0x47, 0x50, 0x48, 0xff, 0x05, 0xca, 0xf5, 0x0c, 0x00};
		static const uint8_t r7[]= {0x83, 0xc0, 0xf8, 0x48, 0x39, 0xf0, 0x76, 0x11, 0x48, 0x8b, 0x47, 0x50, 0x48, 0xff, 0x05, 0xca, 0xf5, 0x0c, 0x00};
		
		static const uint8_t f7p[]= {0x83, 0xc0, 0xfc, 0x48, 0x39, 0xf0, 0x76, 0x11, 0x48, 0x8b, 0x47, 0x50, 0x48, 0xff, 0x05, 0x84, 0x40, 0x08, 0x00};
		static const uint8_t r7p[]= {0x83, 0xc0, 0xf8, 0x48, 0x39, 0xf0, 0x76, 0x11, 0x48, 0x8b, 0x47, 0x50, 0x48, 0xff, 0x05, 0x84, 0x40, 0x08, 0x00};

		
		//hwreg
		static const uint8_t f10[]= {0xe8, 0xaf, 0xe2, 0xff, 0xff, 0x84, 0xc0, 0x74, 0x5b};
		static const uint8_t r10[]= {0xe8, 0xaf, 0xe2, 0xff, 0xff, 0x84, 0xc0, 0xeb, 0x5b};
		
		static const uint8_t f10p[]= {0xe8, 0x9e, 0xf3, 0xff, 0xff, 0x84, 0xc0, 0x74, 0x3d};
		static const uint8_t r10p[]= {0xe8, 0x9e, 0xf3, 0xff, 0xff, 0x84, 0xc0, 0xeb, 0x3d};
		
		//probeportmode
		static const uint8_t f13b[]= {0xff, 0x90, 0x90, 0x01, 0x00, 0x00, 0x49, 0x8b, 0x0e, 0x4c, 0x89, 0xf7, 0x89, 0xc6, 0xff, 0x91, 0x38, 0x01, 0x00, 0x00};
		static const uint8_t r13b[]= {0xc7, 0xc0, 0x01, 0x00, 0x00, 0x00, 0x49, 0x8b, 0x0e, 0x4c, 0x89, 0xf7, 0x89, 0xc6, 0xff, 0x91, 0x38, 0x01, 0x00, 0x00};

		static const uint8_t f13[]= {0xff, 0x91, 0x90, 0x01, 0x00, 0x00, 0x83, 0xf8, 0x02, 0x0f, 0x84, 0xec, 0x00, 0x00, 0x00};
		static const uint8_t r13[]= {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
		
		static const uint8_t f13p[]= {0xff, 0x91, 0x78, 0x01, 0x00, 0x00, 0x83, 0xf8, 0x02, 0x74, 0x64};
		static const uint8_t r13p[]= {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
		
		static const uint8_t f13pb[]= {0xff, 0x90, 0x78, 0x01, 0x00, 0x00, 0x49, 0x8b, 0x0e, 0x4c, 0x89, 0xf7, 0x89, 0xc6, 0xff, 0x91, 0x38, 0x01, 0x00, 0x00};
		static const uint8_t r13pb[]= {0xc7, 0xc0, 0x01, 0x00, 0x00, 0x00, 0x49, 0x8b, 0x0e, 0x4c, 0x89, 0xf7, 0x89, 0xc6, 0xff, 0x91, 0x38, 0x01, 0x00, 0x00};

		
		//getPathByPipe logs
		static const uint8_t f15[]= {0x74, 0x36, 0x48, 0xff, 0x05, 0x7e, 0x51, 0x08, 0x00, 0x44, 0x89, 0x3c, 0x24, 0x48, 0x8d, 0x15, 0x4d, 0x88, 0x03, 0x00, 0x4c, 0x8d, 0x05, 0x28, 0x8a, 0x03, 0x00};
		static const uint8_t r15[]= {0xeb, 0x36, 0x48, 0xff, 0x05, 0x7e, 0x51, 0x08, 0x00, 0x44, 0x89, 0x3c, 0x24, 0x48, 0x8d, 0x15, 0x4d, 0x88, 0x03, 0x00, 0x4c, 0x8d, 0x05, 0x28, 0x8a, 0x03, 0x00};
		
		//getBuiltInPor
		static const uint8_t f16[]= {0x48, 0x89, 0x05, 0xfc, 0x39, 0x12, 0x00, 0x48, 0x8b, 0x83, 0x48, 0x05, 0x00, 0x00, 0xf6, 0x40, 0x14, 0x08, 0x75, 0x0d};
		static const uint8_t r16[]= {0x48, 0x89, 0x05, 0xfc, 0x39, 0x12, 0x00, 0x48, 0x8b, 0x83, 0x48, 0x05, 0x00, 0x00, 0xf6, 0x40, 0x14, 0x08, 0x90, 0x90};
		
		static const uint8_t f16p[]= {0x48, 0x8b, 0x80, 0x48, 0x05, 0x00, 0x00, 0xf6, 0x40, 0x14, 0x08, 0x75, 0x0a};
		static const uint8_t r16p[]= {0x48, 0x8b, 0x80, 0x48, 0x05, 0x00, 0x00, 0xf6, 0x40, 0x14, 0x08, 0x90, 0x90};

		//getHPDState
		static const uint8_t f19[]= {0xbe, 0x70, 0x44, 0x04, 0x00};
		static const uint8_t r19[]= {0xbe, 0xa0, 0x38, 0x16, 0x00};
		
		//savenvram
		static const uint8_t f20[]= {0xff, 0x90, 0xf8, 0x09, 0x00, 0x00, 0x41, 0x89, 0xc6, 0x48, 0x85, 0xdb, 0x74, 0x17};
		static const uint8_t r20[]= {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x48, 0x85, 0xdb, 0x74, 0x17};
		
		static const uint8_t f20p[]= {0xff, 0x90, 0xf8, 0x09, 0x00, 0x00, 0x41, 0x89, 0xc6, 0x48, 0x85, 0xdb, 0x74, 0x17};
		static const uint8_t r20p[]= {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x48, 0x85, 0xdb, 0x74, 0x17};
		
		//SafeForceWake
		static const uint8_t f21[]= {0x0f, 0x84, 0x96, 0x00, 0x00, 0x00, 0x48, 0xff, 0x05, 0xbc, 0x05, 0x0f, 0x00, 0xbe, 0x44, 0x00, 0x13, 0x00, 0x4c, 0x89, 0xf7};
		static const uint8_t r21[]= {0x48, 0xe9, 0x96, 0x00, 0x00, 0x00, 0x48, 0xff, 0x05, 0xbc, 0x05, 0x0f, 0x00, 0xbe, 0x44, 0x00, 0x13, 0x00, 0x4c, 0x89, 0xf7};
		
		static const uint8_t f21p[]= {0x74, 0x3c, 0x48, 0xff, 0x05, 0x7a, 0x73, 0x09, 0x00, 0xbe, 0x44, 0x00, 0x13, 0x00, 0x4c, 0x89, 0xf7, 0xe8, 0x97, 0x80, 0x01, 0x00};
		static const uint8_t r21p[]= {0xeb, 0x3c, 0x48, 0xff, 0x05, 0x7a, 0x73, 0x09, 0x00, 0xbe, 0x44, 0x00, 0x13, 0x00, 0x4c, 0x89, 0xf7, 0xe8, 0x97, 0x80, 0x01, 0x00};
        
        //pixel hwcrt
        static const uint8_t f22[]= {0x48, 0x69, 0xc2, 0x50, 0xc3, 0x00, 0x00, 0x49, 0x89, 0x47, 0x28, 0xbf, 0x08, 0x00, 0x00, 0x00, 0xbe, 0x06, 0x00, 0x00, 0x00, 0xe8, 0x9e, 0x81, 0x01, 0x00, 0x84, 0xc0, 0x74, 0x3a};
        
        static const uint8_t r22[]= {0x48, 0xc7, 0xc0, 0xc0, 0x40, 0xd0, 0x2e, 0x49, 0x89, 0x47, 0x28, 0xbf, 0x08, 0x00, 0x00, 0x00, 0xbe, 0x06, 0x00, 0x00, 0x00, 0xe8, 0x9e, 0x81, 0x01, 0x00, 0x84, 0xc0, 0x90, 0x90};
		

		if (isprod){
			LookupPatchPlus const patchesp[] = {// tgl production kext
				
				{activeKext, f1p, r1p, arrsize(f1p),	1},
				{activeKext, f2p, r2p, arrsize(f2p),	1},
				{activeKext, f2dp, r2dp, arrsize(f2dp),	1},
				{activeKext, f3, r3, arrsize(f3),	1},
				{activeKext, f4, r4, arrsize(f4),	11},
				{activeKext, f4a, r4a, arrsize(f4a),	11},
				{activeKext, f4b, r4b, arrsize(f4b),	2},
				{activeKext, f4c, r4c, arrsize(f4c),	2},
				{activeKext, f7p, r7p, arrsize(f7p),	1},
				{activeKext, f10p, r10p, arrsize(f10p),	1},
				// Keep native probe-port mode flow for compatibility.
				//{activeKext, f13p, r13p, arrsize(f13p),	1},
				//{activeKext, f13pb, r13pb, arrsize(f13pb),	1},
				//{activeKext, f16p, r16p, arrsize(f16p),	1},
				{activeKext, f19, r19, arrsize(f19),	1},
							{activeKext, f20p, r20p, arrsize(f20p),	1},
				
			};
			
			PANIC_COND(!LookupPatchPlus::applyAll(patcher, patchesp , address, size), "ngreen", "kextG11FBT Failed to apply production patches!");
		}
		else {
			LookupPatchPlus const patches[] = {// tgl debug kext
				{activeKext, f1, r1, arrsize(f1),	1},
				{activeKext, f2, r2, arrsize(f2),	1},
				/*{activeKext, f2b, r2b, arrsize(f2b),	1},
				 {activeKext, f2c, r2c, arrsize(f2c),	1},*/
				 {activeKext, f2d, r2d, arrsize(f2d),	1},
				//{activeKext, f2e, r2e, arrsize(f2e),	1},
				{activeKext, f3, r3, arrsize(f3),	1},
				{activeKext, f4, r4, arrsize(f4),	12},
				{activeKext, f4a, r4a, arrsize(f4a),	11},
				{activeKext, f4b, r4b, arrsize(f4b),	2},
				{activeKext, f4c, r4c, arrsize(f4c),	2},
				//{activeKext, f6a, r6a, arrsize(f6a),	1},
				{activeKext, f7, r7, arrsize(f7),	1},
				{activeKext, f10, r10, arrsize(f10),	1},
				// Bypass port probing on spoofed HW (hangs without this)
				{activeKext, f13, r13, arrsize(f13),	1},
				{activeKext, f13b, r13b, arrsize(f13b),	1},
				{activeKext, f15, r15, arrsize(f15),	1},
				//{activeKext, f16, r16, arrsize(f16),	1},
				//{activeKext, f19, r19, arrsize(f19),	1},
			//	{activeKext, f20, r20, arrsize(f20),	1},
				{activeKext, f21, r21, arrsize(f21),	1},
				// Avoid forcing pixel/timing constants in hw CRTC path.
				//{activeKext, f22, r22, arrsize(f22),    1},
				
			};
			
			PANIC_COND(!LookupPatchPlus::applyAll(patcher, patches , address, size), "ngreen", "kextG11FBT Failed to apply dbg patches!");
		}
		
		return true;
		
		
	}     else if (kextG11HW.loadIndex == index) {
		if (this->tglHWLoaded) {
			DBGLOG("ngreen", "Skipping ICL HW — TGL HW already loaded");
			return true;
		}
		auto *activeKext = &kextG11HW;
		DBGLOG("ngreen", "init AppleIntelICLGraphics!");
		NGreen::callback->setRMMIOIfNecessary();
		
		RouteRequestPlus requests[] = {
			 // PAVP/DRM: intercept session command callback (ICL hardware path, shared hook with TGL)
			 {"__ZN16IntelAccelerator19PAVPCommandCallbackE22PAVPSessionCommandID_tjPjb", wrapPavpSessionCallback, this->orgPavpSessionCallback},
			 // getGPUInfo: override topology at ICL object offsets (different from TGL offsets)
			 {"__ZN16IntelAccelerator10getGPUInfoEv", getGPUInfoICL, this->ogetGPUInfoICL},
			 // initHardwareCaps NOT routed: NBlue's wrapper reads TGL offset 0x1120 for SKU,
			 // but ICL stores SKU at 0x1150. Let the original ICL code run — SKU gates are patched.
			 // IGScheduler5resume NOT routed: kIGHwCsDesc is only resolved for kextG11HWT.
			 // With -disablegfxfirmware, Host Preemptive scheduler is selected (not IGScheduler5).
		//last	 {"__ZN12IGScheduler56resumeEv", IGScheduler5resume, this->oIGScheduler5resume},
			 // resetGraphicsEngine NOT routed: NBlue wrapper applies TGL GT workarounds which
			 // target TGL MMIO offsets. Hardware is RPL-P (adlp/raptorlake) — using TGL workarounds
			 // on RPL MMIO could corrupt the command streamer. Let the ICL original run unmodified.
		//last	 {"__ZN20IGHardwareRingBuffer19resetGraphicsEngineEP17IGHardwareContext", resetGraphicsEngine, this->oresetGraphicsEngine},
			 // GuC: remap firmware binary load to match ICL GuC binary path
			 {"__ZN13IGHardwareGuC13loadGuCBinaryEv", loadGuCBinary, this->oloadGuCBinary},
		//last	 {"__ZN13IGHardwareGuC18checkWOPCMSettingsEmR14IOVirtualRange", checkWOPCMSettings, this->ocheckWOPCMSettings},
		//last	 {"__ZN11IGScheduler15canLoadFirmwareEP16IntelAccelerator", canLoadFirmware, this->ocanLoadFirmware},
			 // V36: Hook readAndClearInterrupts to initialize Gen11 multi-engine GT interrupts.
			 // Same implementation as TGL path — Gen11 IRQ registers are identical for ICL/TGL.
			 // V37: DISABLED — caused boot hang on TGL path; disabling ICL too for safety.
			 // {"__ZN16IntelAccelerator23readAndClearInterruptsEPv", readAndClearInterrupts, this->oreadAndClearInterrupts},
		 };

		PANIC_COND(!RouteRequestPlus::routeAll(patcher, index, requests, address, size), "ngreen","Failed to route dp symbols");
		
		// SKU gate 1+2: NOP JNZ/JA + XOR eax,eax (Sonoma AppleIntelICLGraphics, verified in KC)
		static const uint8_t fSKUGates12[] = {
			0x83, 0xF9, 0x01,
			0x0F, 0x85, 0x0B, 0x01, 0x00, 0x00,
			0xFF, 0xC8,
			0x83, 0xF8, 0x07,
			0x0F, 0x87, 0x00, 0x01, 0x00, 0x00,
			0x48, 0x8D, 0x0D, 0x77, 0x02, 0x00, 0x00, 0x48
		};
		static const uint8_t rSKUGates12[] = {
			0x83, 0xF9, 0x01,
			0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
			0x31, 0xC0,
			0x83, 0xF8, 0x07,
			0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
			0x48, 0x8D, 0x0D, 0x77, 0x02, 0x00, 0x00, 0x48
		};

		// SKU gate 3: NOP JNZ (Sonoma AppleIntelICLGraphics, verified in KC)
		static const uint8_t fSKUGate3[] = {
			0x83, 0xF8, 0x08, 0x0F, 0x85, 0xC2, 0x00, 0x00, 0x00, 0xC7
		};
		static const uint8_t rSKUGate3[] = {
			0x83, 0xF8, 0x08, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0xC7
		};

		// SKU bypass: TEST rax,rax; JZ->JMP (Sonoma, f2Long verified in KC at 0x14152bcd)
		static const uint8_t fSkuBypassLong[] = {
			0x48, 0x85, 0xC0, 0x74, 0x72, 0x48, 0x0F, 0xBC, 0xC0, 0x48, 0xFF, 0xC0, 0x48,
			0x8D, 0x15, 0x00, 0xCD, 0x0F, 0x00, 0x48, 0x8D, 0x48, 0xFF, 0x48, 0xF7, 0xC1,
			0xFD, 0xFF, 0xFF, 0xFF, 0x74, 0x27, 0x48, 0x6B, 0xC9, 0x79
		};
		static const uint8_t rSkuBypassLong[] = {
			0x48, 0x85, 0xC0, 0xEB, 0x72, 0x48, 0x0F, 0xBC, 0xC0, 0x48, 0xFF, 0xC0, 0x48,
			0x8D, 0x15, 0x00, 0xCD, 0x0F, 0x00, 0x48, 0x8D, 0x48, 0xFF, 0x48, 0xF7, 0xC1,
			0xFD, 0xFF, 0xFF, 0xFF, 0x74, 0x27, 0x48, 0x6B, 0xC9, 0x79
		};

		LookupPatchPlus const patches[] = {
			{&kextG11HW, fSKUGates12,    rSKUGates12,    arrsize(fSKUGates12),    1},
			{&kextG11HW, fSKUGate3,      rSKUGate3,      arrsize(fSKUGate3),      1},
			{&kextG11HW, fSkuBypassLong, rSkuBypassLong, arrsize(fSkuBypassLong), 1},
		};
		
		/*auto catalina = getKernelVersion() == KernelVersion::Catalina;
		if (catalina)
			PANIC_COND(!LookupPatchPlus::applyAll(patcher, patchesc , address, size), "ngreen", "cata Failed to apply patches!");
		else*/
		for (size_t i = 0; i < sizeof(patches)/sizeof(patches[0]); ++i) {
			//IOSleep(delay);
			PANIC_COND(!patches[i].apply(patcher, address, size), "ngreen", "kextG11HW Failed to apply patch %zu", i);
		}
		DBGLOG("ngreen", "Loaded AppleIntelICLGraphics!");

		return true;

    } else if (kextG11HWT.loadIndex == index) {
		this->tglHWLoaded = true;
		auto *activeKext = &kextG11HWT;
		SYSLOG("ngreen", "init AppleIntelTGLGraphics (HW accelerator)");
		NGreen::callback->setRMMIOIfNecessary();
/*
		SolveRequestPlus solveRequests[] = {
			
			{"__ZN11IGAccelTask16getBlit2DContextEb", this->ogetBlit3DContext},
		};
		SYSLOG_COND(!SolveRequestPlus::solveAll(patcher, index, solveRequests, address, size), "ngreen",	"Failed to resolve symbols");
		 */
		
		 RouteRequestPlus requests[] = {
			 
			 {"__ZN16IntelAccelerator20_PAVPCommandCallbackEP8OSObject22PAVPSessionCommandID_tjPj", wrapPavpSessionCallback, this->orgPavpSessionCallback},
			 
			 {"__ZN13IGHardwareGuC13loadGuCBinaryEv", loadGuCBinary, this->oloadGuCBinary},
			 
			 {"__ZN16IntelAccelerator10getGPUInfoEv", getGPUInfo, this->ogetGPUInfo},
			
			 // resetGraphicsEngine: apply RPL GT workarounds (Wa_14011060649, Wa_14011059788, Wa_14015795083)
			 // before the original TGL engine reset. Without these, GPU hangs on first 3D command.
			 {"__ZN20IGHardwareRingBuffer19resetGraphicsEngineEP17IGHardwareContext", resetGraphicsEngine, this->oresetGraphicsEngine},
			 
			 // ForceWake: replace Apple's SafeForceWakeMultithreaded with i915-ported version.
			 // Apple's code uses 90ms timeouts and no fallback; ours uses 50ms + reserve-bit fallback.
			 // The original domain mapping was broken (d<<1 loop misaligned Apple's 3-bit dom bitmap).
			 {"__ZN16IntelAccelerator26SafeForceWakeMultithreadedEbjj", forceWake, this->oforceWake},
			 
			 // start: inject MultiForceWakeSelect=1 into Development dictionary BEFORE original start.
			 // This tells the accelerator to use our hooked SafeForceWakeMultithreaded instead of
			 // the framebuffer's SafeForceWake (which fails on RPL-P with ForceWake ACK=0).
			 {"__ZN16IntelAccelerator5startEP9IOService", start, this->ostart},

			 // V36: Hook readAndClearInterrupts to initialize Gen11 multi-engine GT interrupts.
			 // Without this, RCS/BCS user interrupts and context-switch notifications may not
			 // be properly enabled, preventing IOAccelF2 from seeing stamp completions.
			 // V37: DISABLED — caused boot hang (symbol may not exist in TGL kext, or
			 // interrupt reprogramming too early causes deadlock/panic).
			 // {"__ZN16IntelAccelerator23readAndClearInterruptsEPv", readAndClearInterrupts, this->oreadAndClearInterrupts},

			// {"__ZN11IGAccelTask16getBlit3DContextEb", getBlit3DContext, this->ogetBlit3DContext},
		
			 //{"__Z31blit3d_initialize_scratch_spaceP16IGAccelSysMemory", blit3d_initialize_scratch_space, this->oblit3d_initialize_scratch_space},
			 //{"__Z15blit3d_init_ctxP23IGHardwareBlit3DContext", blit3d_init_ctx, this->oblit3d_init_ctx},
			 //{"__ZN23IGHardwareBlit3DContext10initializeEv", IGHardwareBlit3DContextinitialize, this->oIGHardwareBlit3DContextinitialize},
			// {"__ZNK14IGMappedBuffer9getMemoryEv", IGMappedBuffergetMemory, this->oIGMappedBuffergetMemory},
			 
		 };
		PANIC_COND(!RouteRequestPlus::routeAll(patcher, index, requests, address, size), "ngreen","Failed to route symbols");
		
		// SKU/device-ID panic bypass (verified @ 0x23c1d in LE binary)
		// Original: mov edi,[rsi]; cmp edi,0xDEAFBEEE; jg sentinel; cmp edi,0x9A408086; je ok; cmp edi,0x9A488086; je ok; call panic
		// Patch:    nop the sentinel-jg + change last "je ok" → "jmp ok" → always jumps to GT2 init path.
		// Necessary: our spoofed 0x9A498086 is not in the whitelist (0x9A408086 / 0x9A488086).
		static const uint8_t f3[] = {
			0x8b, 0x3e, 0x81, 0xff, 0xee, 0xbe, 0xaf, 0xde, 0x7f, 0x15, 0x81, 0xff, 0x86, 0x80, 0x40, 0x9a, 0x74, 0x2d
		};
		static const uint8_t r3[] = {
			0x8b, 0x3e, 0x81, 0xff, 0xee, 0xbe, 0xaf, 0xde, 0x90, 0x90, 0x81, 0xff, 0x86, 0x80, 0x40, 0x9a, 0xeb, 0x2d
		};
		// GT tier override: stores GT1 (0x1) instead of GT2 (0x2) at IGAccelDevice+0x1120.
		// Disabled – 0x9A49 is GT2 so the default path (which writes 0x2) is correct.
		static const uint8_t f3a[] = {//gt1
			0x41, 0xc7, 0x86, 0x20, 0x11, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0xe9, 0xda, 0xfc, 0xff, 0xff
		};
		static const uint8_t r3a[] = {
			0x41, 0xc7, 0x86, 0x20, 0x11, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xe9, 0xda, 0xfc, 0xff, 0xff
		};
		
		// L3BankCount bypass (verified @ 0x28776 in LE binary)
		// Original: topology-gated conditionals (cmp slices/eu/threads) → only set L3BankCount=8 for a specific config.
		// Patch:    NOP all conditional branches → always store L3BankCount=8 @ IGAccelDevice+0x1164.
		static const uint8_t f3b[] = {// jmp L3BankCount
			0x74, 0x23, 0x83, 0xf9, 0x02, 0x0f, 0x85, 0x89, 0x01, 0x00, 0x00, 0x83, 0xfe, 0x01, 0x75, 0x59, 0x83, 0xfa, 0x0c, 0x75, 0x54, 0x41, 0xc7, 0x87, 0x64, 0x11, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00
		};
		static const uint8_t r3b[] = {
			0x90, 0x90, 0x83, 0xf9, 0x02, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x83, 0xfe, 0x01, 0x90, 0x90, 0x83, 0xfa, 0x0c, 0x90, 0x90, 0x41, 0xc7, 0x87, 0x64, 0x11, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00
		};
		
		// MaxEUPerSubSlice override (verified @ 0x28692 in LE binary)
		// Original: MaxEUPerSubSlice = 8 - popcount(EUDisableFuse)  → stores result at IGAccelDevice+0x116c
		// Patch:    hardcodes MaxEUPerSubSlice=8 for RPL 96EU (8 EU per traditional sub-slice).
		//           TGL binary counts sub-slices (SS), not dual sub-slices (DSS).
		//           Linux shows 16 EU/DSS = 8 EU/SS since each DSS has 2 SS.
		static const uint8_t f3bb[] = {//MaxEUPerSubSlice
			0xbe, 0x08, 0x00, 0x00, 0x00, 0x29, 0xde, 0x41, 0x89, 0xb7, 0x6c, 0x11, 0x00, 0x00, 0x41, 0x8b, 0x8f, 0x58, 0x11, 0x00, 0x00
		};
		static const uint8_t r3bb[] = {
			0xbe, 0x08, 0x00, 0x00, 0x00, 0x90, 0x90, 0x41, 0x89, 0xb7, 0x6c, 0x11, 0x00, 0x00, 0x41, 0x8b, 0x8f, 0x58, 0x11, 0x00, 0x00
		};
		
		// NumSubSlices override (verified @ 0x28654 in LE binary)
		// Original: mov ebx,[rbp-0x30]; popcnt esi,ebx; add esi,esi; mov [r15+0x1158],esi
		//           → NumSubSlices = popcount(subsliceMask) * 2  (hardware-detected)
		// Patch:    hardcodes NumSubSlices=12 for RPL 96EU (6 DSS × 2 SS/DSS = 12 SS).
		//           Linux i915: subslice total=6 mask=0x3f, so 6 DSS doubled to 12 SS.
		static const uint8_t f3bbb[] = {//NumSubSlices
			0x8b, 0x5d, 0xd0, 0xf3, 0x0f, 0xb8, 0xf3, 0x01, 0xf6, 0x41, 0x89, 0xb7, 0x58, 0x11, 0x00, 0x00
		};
		static const uint8_t r3bbb[] = {
			0x8b, 0x5d, 0xd0, 0xbe, 0x0c, 0x00, 0x00, 0x00, 0x90, 0x41, 0x89, 0xb7, 0x58, 0x11, 0x00, 0x00
		};
		
		// GPU caps override (disabled) – would change MaxSlices 6→5, SARation 2→1, MaxEU/SS 6→5.
		// Leave disabled unless acceleration shows wrong Metal tier/feature set.
		static const uint8_t f4[] = {// CAPS
			0xc7, 0x83, 0x48, 0x11, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x8b, 0x83, 0x58, 0x11, 0x00, 0x00, 0xd1, 0xe8, 0xba, 0x02, 0x00, 0x00, 0x00, 0xbe, 0x06, 0x00, 0x00, 0x00
		};
		static const uint8_t r4[] = {
			0xc7, 0x83, 0x48, 0x11, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x8b, 0x83, 0x58, 0x11, 0x00, 0x00, 0x90, 0x90, 0xba, 0x01, 0x00, 0x00, 0x00, 0xbe, 0x05, 0x00, 0x00, 0x00
		};

		

		{
			LookupPatchPlus const patches[] = {
				
				//{activeKext, f3a, r3a, arrsize(f3a),	1},
				
				{activeKext, f3, r3, arrsize(f3),	1},
				{activeKext, f3b, r3b, arrsize(f3b),	1},
				
				{activeKext, f3bb, r3bb, arrsize(f3bb),	1},
				 {activeKext, f3bbb, r3bbb, arrsize(f3bbb),	1},
				 
				// {activeKext, f4, r4, arrsize(f4),	1},
				
			};
			
			
			PANIC_COND(!LookupPatchPlus::applyAll(patcher, patches , address, size), "ngreen", "kextG11HWT Failed to apply patches!");
		}
		
		SYSLOG("ngreen", "Loaded AppleIntelTGLGraphics! slices=1 subslices=12(6DSS) maxEU/SS=8 totalEU=96 L3=8");

		return true;
	}

    return false;
}

int Gen11::blit3d_supported()
{
	return 0;
	
}


void  Gen11::setAsyncSliceCount(void *that,uint32_t configRaw)
{
		uint32_t sliceCount     = (configRaw >> 0) & 0xFF;
		uint32_t subsliceCount  = (configRaw >> 8) & 0xFF;
		uint32_t euCount        = (configRaw >> 16) & 0xFF;

	uint32_t sliceField = 0;
		switch (sliceCount) {
			case 1: sliceField = 1; break;
			case 2: sliceField = 2; break;
			default:
				panic("IGPU: setAsyncSliceCount - Invalid slice count: %u\n", sliceCount);
				break;
		}
		
		uint32_t subsliceField = 0;
		switch (subsliceCount) {
			case 2: subsliceField = 0x20; break;
			case 4: subsliceField = 0x40; break;
			case 5: subsliceField = 0x50; break;
			case 6: subsliceField = 0x60; break;
			case 8: subsliceField = 0x80; break;
			default:
						panic("IGPU: setAsyncSliceCount - Invalid subsliceCount: %u\n", subsliceCount);
						break;
		}

		uint32_t euField = 0;
		switch (euCount) {
			case 1: euField = 0x100; break;
			case 2: euField = 0x200; break;
			case 3: euField = 0x300; break;
			case 4: euField = 0x400; break;
			case 5: euField = 0x500; break;
			case 6: euField = 0x600; break;
			case 8: euField = 0x800; break;
			default:
						panic("IGPU: setAsyncSliceCount - Invalid EU count/power mode: %u\n", euCount);
						break;
		}

		uint32_t hwRegisterValue = sliceField | subsliceField | euField;

		getMember<uint32_t>(that, 0x12a0) = configRaw;


		//SafeForceWake(that,true, 4);
		volatile uint32_t* mmioBase = getMember<volatile uint32_t*>(that, 0x1240);
		mmioBase[0xa204 / 4] = hwRegisterValue;
		//SafeForceWake(that,false, 4);
	
}


	static const uint8_t DAT_000b0bb0[] = {
		0x00, 0x36, 0x6e, 0x01, 0x00, 0xf8, 0x24, 0x01,
		0x00, 0xf0, 0x49, 0x02, 0x40, 0x78, 0x7d, 0x01
	};


bool  Gen11::getGPUInfo(void *that)
{
	
#define RPM_CONFIG0				(0xd00)
#define   GEN9_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_SHIFT	3
#define   GEN9_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_MASK	(1 << GEN9_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_SHIFT)
#define   GEN9_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_19_2_MHZ	0
#define   GEN9_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_24_MHZ	1
#define   GEN11_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_SHIFT	3
#define   GEN11_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_MASK	(0x7 << GEN11_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_SHIFT)
#define   GEN11_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_24_MHZ	0
#define   GEN11_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_19_2_MHZ	1
#define   GEN11_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_38_4_MHZ	2
#define   GEN11_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_25_MHZ	3
#define   GEN10_RPM_CONFIG0_CTC_SHIFT_PARAMETER_SHIFT	1
#define   GEN10_RPM_CONFIG0_CTC_SHIFT_PARAMETER_MASK	(0x3 << GEN10_RPM_CONFIG0_CTC_SHIFT_PARAMETER_SHIFT)
	
	auto ret=FunctionCast(getGPUInfo, callback->ogetGPUInfo)(that);
	
	// --- GPU topology override for RPL i7-13700H (verified from Linux i915 syslog) ---
	// Linux i915 reports: 1 slice, 6 DSS (mask=0x3f), 16 EU/DSS, 96 EU total.
	// TGL binary uses traditional sub-slices (SS), not dual sub-slices (DSS):
	//   6 DSS × 2 SS/DSS = 12 SS,  16 EU/DSS / 2 = 8 EU/SS, 12 × 8 = 96 EU.
	// Object layout (byte offsets from `this`, verified via disassembly):
	//   0x115c = NumSlices          0x0dd8 = NumSlices mirror
	//   0x1158 = NumSubSlices       0x0ddc = NumSubSlices mirror
	//   0x116c = MaxEUPerSubSlice
	//   0x1124 = ExecutionUnitCount (= MaxEUPerSubSlice × NumSubSlices)
	//   0x1150 = Frequency pair (low32=fMaxMHz, high32=fMinMHz)
	//   0x1164 = L3BankCount
	unsigned int numSlices        = 1;
	unsigned int numSubSlices     = 12;  // 6 DSS × 2 = 12 traditional SS
	unsigned int maxEUPerSubSlice = 8;   // 16 EU/DSS ÷ 2 SS/DSS = 8 EU/SS
	unsigned int totalEU          = maxEUPerSubSlice * numSubSlices; // = 96
	
	getMember<UInt32>(that, 0x115c) = numSlices;
	getMember<UInt32>(that, 0x1158) = numSubSlices;
	getMember<UInt32>(that, 0x116c) = maxEUPerSubSlice;
	getMember<UInt32>(that, 0x1124) = totalEU;
	getMember<UInt32>(that, 0x0dd8) = numSlices;
	getMember<UInt32>(that, 0x0ddc) = numSubSlices;
	getMember<UInt32>(that, 0x1164) = 8;  // L3BankCount (confirmed from InsanelyMac TGL logs)
	
	// Frequency: fMaxFrequencyInMhz=1000, fMinFrequencyInMhz=450 (TGL defaults)
	getMember<uint64_t>(that, 0x1150) = 0x1C2000003E8ULL;
	
	SYSLOG("ngreen", "getGPUInfo: overridden topology → slices=%u subslices=%u maxEU/SS=%u totalEU=%u L3Banks=8",
		   numSlices, numSubSlices, maxEUPerSubSlice, totalEU);
			
	return ret;
}


bool Gen11::getGPUInfoICL(void *that)
{
	auto ret = FunctionCast(getGPUInfoICL, callback->ogetGPUInfoICL)(that);
	
	// --- GPU topology override for ICL HW binary ---
	// ICL object layout (verified from AppleIntelICLGraphics.sonoma.bin disassembly):
	//   0x1190 = NumSlices          0x12cc = NumSlices mirror
	//   0x1188 = NumSubSlices       0x12d0 = NumSubSlices mirror
	//   0x11a0 = MaxEUPerSubSlice
	//   0x1154 = ExecutionUnitCount (= MaxEUPerSubSlice × NumSubSlices)
	//   0x1198 = L3BankCount
	//   0x1150 = GPU Sku
	// ICL counts traditional sub-slices (same as TGL binary).
	// Use ICL GT2 LP config (1×8×8 = 64 EU) to stay within ICL-valid topology.
	unsigned int numSlices        = 1;
	unsigned int numSubSlices     = 8;   // ICL GT2 LP max (8 SS)
	unsigned int maxEUPerSubSlice = 8;
	unsigned int totalEU          = maxEUPerSubSlice * numSubSlices; // = 64
	
	getMember<UInt32>(that, 0x1190) = numSlices;
	getMember<UInt32>(that, 0x1188) = numSubSlices;
	getMember<UInt32>(that, 0x11a0) = maxEUPerSubSlice;
	getMember<UInt32>(that, 0x1154) = totalEU;
	getMember<UInt32>(that, 0x12cc) = numSlices;     // NumSlices mirror
	getMember<UInt32>(that, 0x12d0) = numSubSlices;  // NumSubSlices mirror
	getMember<UInt32>(that, 0x1198) = 8;             // L3BankCount
	
	SYSLOG("ngreen", "getGPUInfoICL: overridden topology → slices=%u subslices=%u maxEU/SS=%u totalEU=%u L3Banks=8",
		   numSlices, numSubSlices, maxEUPerSubSlice, totalEU);
	
	return ret;
}


bool Gen11::initHardwareCaps(void *this_ptr) {
		uint32_t gpuSku = getMember<uint32_t>(this_ptr, 0x1120);
		bool result = false;
		
		uint32_t uVar1;
		int iVar2;
		int iVar3;
		int iVar4;

		if (gpuSku == 2) {
			// --- SKU 2 (TGLLP) - Original TGL values for 6 Dual SubSlices (12 SubSlices) ---
					
					// Buffer sizes for 12 SubSlices (6 DSS × 2 SS/DSS)
					// 0xc0 (192) = 16 bytes × 12 SS
					getMember<uint64_t>(this_ptr, 0x112c) = 0x222000000c0ULL;
					
					// 0x150 (336) = 28 bytes × 12 SS
					getMember<uint64_t>(this_ptr, 0x1134) = 0x22200000150ULL;
					getMember<uint32_t>(this_ptr, 0x113c) = 0x150;
					
					getMember<uint64_t>(this_ptr, 0x1174) = 0x200000007ULL;
					getMember<uint64_t>(this_ptr, 0x117c) = 0x1000000080ULL;
					
					getMember<uint32_t>(this_ptr, 0x1160) = 0xf00;
					
					// Max Dual SubSlices = 6 (matches hardware: 6 DSS)
					getMember<uint32_t>(this_ptr, 0x1148) = 0x6;
					
					// Calculate actual Dual SubSlices (SubSlices / 2)
					uVar1 = getMember<uint32_t>(this_ptr, 0x1158) >> 1;
					iVar3 = 2;
					
					// Reference Dual SubSlices count = 6 (must match max to avoid underflow)
					iVar4 = 0x6;
					iVar2 = 0x80;
		}
		else {
			if (gpuSku != 1) {
				result = false;
				return result;
			}
			
			// --- SKU 1 (TGLHP) - Modified for 5 DSS ---
			
			// Sizes for 10 SubSlices
			getMember<uint64_t>(this_ptr, 0x112c) = 0x2d800000140ULL;
			getMember<uint64_t>(this_ptr, 0x1134) = 0x27000000168ULL;
			getMember<uint32_t>(this_ptr, 0x113c) = 0x168;
			
			getMember<uint64_t>(this_ptr, 0x1174) = 0x100000007ULL;
			getMember<uint64_t>(this_ptr, 0x117c) = 0x1000000040ULL;
			
			getMember<uint32_t>(this_ptr, 0x1160) = 0x800;
			
			// Max SubSlices set to 10
			getMember<uint32_t>(this_ptr, 0x1148) = 0xA;
			
			uVar1 = getMember<uint32_t>(this_ptr, 0x1158);
			iVar3 = 1;
			
			// Reference count set to 10
			iVar4 = 0xA;
			iVar2 = 0x40;
		}

		// Final calculations
		getMember<uint32_t>(this_ptr, 0x114c) = uVar1;
		
		uint8_t &byteRef = getMember<uint8_t>(this_ptr, 0x1184);
		byteRef = byteRef & 0xFB;
		
		getMember<uint32_t>(this_ptr, 0x1128) = iVar3 * uVar1;
		getMember<uint32_t>(this_ptr, 0x1144) = (iVar4 - uVar1) * iVar3;
		getMember<uint32_t>(this_ptr, 0x1140) = iVar2 * uVar1;
		
		getMember<uint32_t>(this_ptr, 0x1168) = getMember<uint32_t>(this_ptr, 0x115c) << 4;
		
		result = true;
		return result;
	}

void Gen11::checkWOPCMSettings(void *that,unsigned long param_1,void *param_2)
{
	const uint32_t GUC_WOPCM_OFFSET = 1 * 1024 * 1024;
	const uint32_t GUC_WOPCM_SIZE = 1 * 1024 * 1024;
	
	typedef struct {
		uint64_t address;
		uint64_t length;
	} IOVirtualRange;
	
	IOVirtualRange *wopcm_range = (IOVirtualRange *)param_2;
		
	wopcm_range->address = GUC_WOPCM_OFFSET;
	wopcm_range->length = GUC_WOPCM_SIZE;
	
}

void *ccont;

IOReturn Gen11::wrapPavpSessionCallback( void *intelAccelerator, int32_t sessionCommand, uint32_t sessionAppId, uint32_t *a4, bool flag) {
	
	getMember<void *>(intelAccelerator, 0x90) = ccont;

	//void* pPavpContext = *getMember<void**>(intelAccelerator, 0x1278);
	//void* pStampTrackingStruct = *(void**)getMember<char*>(pPavpContext, 0xb8);
	
	if (sessionCommand == 4) {
		//return kIOReturnTimeout;
		return kIOReturnSuccess;
	}

	return FunctionCast(wrapPavpSessionCallback, callback->orgPavpSessionCallback)(intelAccelerator, sessionCommand, sessionAppId, a4, flag);
}

IOReturn Gen11::wrapFBClientDoAttribute(void *fbclient, uint32_t attribute, unsigned long *unk1, unsigned long unk2, unsigned long *unk3, unsigned long *unk4,  void *externalMethodArguments) {
	if (attribute == 0x923) {
		return kIOReturnUnsupported;
	}
	return FunctionCast(wrapFBClientDoAttribute, callback->orgFBClientDoAttribute)(fbclient, attribute, unk1, unk2, unk3, unk4,  externalMethodArguments);
}

unsigned long Gen11::loadGuCBinary(void *that) {
	// Stub: return 1 (success) without loading firmware.
	// RPL cannot authenticate TGL GuC firmware. Use -disablegfxfirmware boot arg
	// to force host-based scheduling, which skips GuC entirely.
	SYSLOG("ngreen", "loadGuCBinary: stubbed to return 1 (use -disablegfxfirmware)");
	return 1;
}

UInt8 Gen11::wrapLoadGuCBinary(void *that) {

	if (callback->firmwareSizePointer)
		callback->performingFirmwareLoad = true;

	auto r = FunctionCast(wrapLoadGuCBinary, callback->orgLoadGuCBinary)(that);
	DBGLOG("ngreen", "loadGuCBinary returned %d", r);

	callback->performingFirmwareLoad = false;

	return r;
}

bool Gen11::wrapLoadFirmware(void *that) {

	//(*reinterpret_cast<uintptr_t **>(that))[35] = reinterpret_cast<uintptr_t>(wrapSystemWillSleep);
	//(*reinterpret_cast<uintptr_t **>(that))[36] = reinterpret_cast<uintptr_t>(wrapSystemDidWake);
	return FunctionCast(wrapLoadFirmware, callback->orgLoadFirmware)(that);
}

void Gen11::wrapSystemWillSleep(void *that) {
	DBGLOG("ngreen", "systemWillSleep GuC callback");
}

void Gen11::wrapSystemDidWake(void *that) {
	DBGLOG("ngreen", "systemDidWake GuC callback");

	// This is IGHardwareGuC class instance.
	auto &GuC = (reinterpret_cast<OSObject **>(that))[76];

	if (GuC)
	if (GuC->metaCast("IGHardwareGuC")) {
		DBGLOG("igfx", "reloading firmware on wake; discovered IGHardwareGuC - releasing");
		GuC->release();
		GuC = nullptr;
	}

	FunctionCast(wrapLoadFirmware, callback->orgLoadFirmware)(that);
}

bool Gen11::wrapInitSchedControl(void *that) {
	DBGLOG("ngreen", "attempting to init sched control with load %d", callback->performingFirmwareLoad);
	bool perfLoad = callback->performingFirmwareLoad;
	callback->performingFirmwareLoad = false;
	bool r = FunctionCast(wrapInitSchedControl, callback->orgInitSchedControl)(that);

	callback->performingFirmwareLoad = perfLoad;
	return r;
}

void *Gen11::wrapIgBufferWithOptions(void *accelTask, void* size, unsigned int type, unsigned int flags) {
	void *r = nullptr;

	if (callback->performingFirmwareLoad) {
		callback->dummyFirmwareBuffer = Buffer::create<uint8_t>(*(unsigned long*)size);

		const void *fw = nullptr;
		const void *fwsig = nullptr;
		size_t fwsize = 0;
		size_t fwsigsize = 0;


		/*fw = GuCFirmwareKBL;
		fwsig = GuCFirmwareKBLSignature;
		fwsize = GuCFirmwareKBLSize;
		fwsigsize = GuCFirmwareSignatureSize;*/

		unsigned long newsize = fwsize > *(unsigned long*)size ? ((fwsize + 0xFFFF) & (~0xFFFF)) : *(unsigned long*)size;
		r = FunctionCast(wrapIgBufferWithOptions, callback->orgIgBufferWithOptions)(accelTask, (void*)newsize,type,flags);
		if (r && callback->dummyFirmwareBuffer) {
			auto status = MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock);
			if (status == KERN_SUCCESS) {
				callback->realFirmwareBuffer = static_cast<uint8_t **>(r)[7];
				static_cast<uint8_t **>(r)[7] = callback->dummyFirmwareBuffer;
				lilu_os_memcpy(callback->realFirmwareBuffer, fw, fwsize);
				lilu_os_memcpy(callback->signaturePointer, fwsig, fwsigsize);
				callback->realBinarySize = static_cast<uint32_t>(fwsize);
				*callback->firmwareSizePointer = static_cast<uint32_t>(fwsize);
				MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
			} else {
				//SYSLOG("igfx", "ig buffer protection upgrade failure %d", status);
			}
		} else if (callback->dummyFirmwareBuffer) {
			//SYSLOG("igfx", "ig shared buffer allocation failure");
			Buffer::deleter(callback->dummyFirmwareBuffer);
			callback->dummyFirmwareBuffer = nullptr;
		} else {
			//SYSLOG("igfx", "dummy buffer allocation failure");
		}
	} else {
		r = FunctionCast(wrapIgBufferWithOptions, callback->orgIgBufferWithOptions)(accelTask, size,type,flags);
	}

	return r;
}

UInt64 Gen11::wrapIgBufferGetGpuVirtualAddress(void *that) {
	if (callback->performingFirmwareLoad && callback->realFirmwareBuffer) {
		static_cast<uint8_t **>(that)[7] = callback->realFirmwareBuffer;
		callback->realFirmwareBuffer = nullptr;
		Buffer::deleter(callback->dummyFirmwareBuffer);
		callback->dummyFirmwareBuffer = nullptr;
	}

	return FunctionCast(wrapIgBufferGetGpuVirtualAddress, callback->orgIgBufferGetGpuVirtualAddress)(that);
}


uint32_t Gen11::wrapReadRegister32(void *controller, uint32_t address) {
	if (controller == nullptr)
		return NGreen::callback->readReg32(address);  // readReg32 now takes byte offsets

	// Mirror Apple bounds logic, but keep a fallback path for the 2D-only dropped window.
	auto partInfo = getMember<uint8_t *>(controller, 0xCF8);
	auto mmioBase = getMember<uint8_t *>(controller, 0x9B8);
	auto mmioSize = static_cast<uint32_t>(getMember<int>(controller, 0xC38));

	bool twoDOnlyPart = partInfo && ((partInfo[0xB2] & 0x1) != 0);
	bool dropped2DWindow = (address >= 0x2000U && address <= 0x23FFFFU);

	if (twoDOnlyPart && dropped2DWindow) {
		return NGreen::callback->readReg32(address);  // readReg32 now takes byte offsets
	}

	if (mmioBase && mmioSize >= 4U && address < (mmioSize - 4U)) {
		return *reinterpret_cast<volatile uint32_t *>(mmioBase + address);
	}

	return FunctionCast(wrapReadRegister32, callback->owrapReadRegister32)(controller, address);
}

void Gen11::wrapWriteRegister32(void *controller, uint32_t address, uint32_t value) {
	if (controller == nullptr) {
		NGreen::callback->writeReg32(address, value);  // readReg32 now takes byte offsets
		return;
	}
	FunctionCast(wrapWriteRegister32, callback->owrapWriteRegister32)(controller,address,value);
}


void Gen11::sanitizeCDClockFrequency(void *that) {

	//auto referenceFrequency = callback->wrapReadRegister32(that, SKL_DSSM) & ICL_DSSM_CDCLK_PLL_REFCLK_MASK;
	auto referenceFrequency =NGreen::callback->readReg32(ICL_REG_DSSM)>> 29;
	//auto referenceFrequency = callback->wrapReadRegister32(that, ICL_REG_DSSM) >> 29;
	uint32_t newCdclkFrequency = 0;
	uint32_t newPLLFrequency = 0;
	switch (referenceFrequency) {
		case ICL_REF_CLOCK_FREQ_19_2:
			newCdclkFrequency = ICL_CDCLK_FREQ_652_8;
			newPLLFrequency = ICL_CDCLK_PLL_FREQ_REF_19_2;
			break;
			
		case ICL_REF_CLOCK_FREQ_24_0:
			newCdclkFrequency = ICL_CDCLK_FREQ_648_0;
			newPLLFrequency = ICL_CDCLK_PLL_FREQ_REF_24_0;
			break;
			
		case ICL_REF_CLOCK_FREQ_38_4:
			newCdclkFrequency = ICL_CDCLK_FREQ_652_8;
			newPLLFrequency = ICL_CDCLK_PLL_FREQ_REF_38_4;
			break;
			
		default:
			return;
	}

	DBGLOG("ngreen", "sanitizeCDClockFrequency: ref=%u targetCdclk=0x%x targetPll=0x%x", referenceFrequency, newCdclkFrequency, newPLLFrequency);

	// Use solved original directly so sanitize remains safe even when disableCDClock route is toggled off.
	if (callback->orgDisableCDClock) {
		callback->orgDisableCDClock(that);
	} else {
		disableCDClock(that);
	}

	callback->orgSetCDClockFrequency(that, newPLLFrequency);

}
/*void Gen11::sanitizeCDClockFrequency(void *that) {

	//auto referenceFrequency = callback->wrapReadRegister32(that, SKL_DSSM) & ICL_DSSM_CDCLK_PLL_REFCLK_MASK;
	auto referenceFrequency =NGreen::callback->readReg32(ICL_REG_DSSM)>> 29;
	//auto referenceFrequency = callback->wrapReadRegister32(that, ICL_REG_DSSM) >> 29;
	uint32_t newCdclkFrequency = 0;
	uint32_t newPLLFrequency = 0;
	switch (referenceFrequency) {
		case ICL_REF_CLOCK_FREQ_19_2:
			newCdclkFrequency = ICL_CDCLK_FREQ_652_8;
			newPLLFrequency = ICL_CDCLK_PLL_FREQ_REF_19_2;
			break;
			
		case ICL_REF_CLOCK_FREQ_24_0:
			newCdclkFrequency = ICL_CDCLK_FREQ_648_0;
			newPLLFrequency = ICL_CDCLK_PLL_FREQ_REF_24_0;
			break;
			
		case ICL_REF_CLOCK_FREQ_38_4:
			newCdclkFrequency = ICL_CDCLK_FREQ_652_8;
			newPLLFrequency = ICL_CDCLK_PLL_FREQ_REF_38_4;
			break;
			
		default:
			return;
	}
	
	disableCDClock(that);
	
	callback->orgSetCDClockFrequency(that, newPLLFrequency);
}*/

uint32_t Gen11::wrapProbeCDClockFrequency(void *that) {

	// Sonoma probeCDClockFrequency checks reg 0x46070 (BXT_DE_PLL_ENABLE) bit 31 first.
	// If bit 31 is CLEAR it panics immediately: "Wrong CD clock frequency set by EFI".
	// EFI on Hackintosh may leave this clear, so we ensure it is set before calling the original.
	auto squash = NGreen::callback->readReg32(BXT_DE_PLL_ENABLE);  // byte addr OK — readReg32 divides internally
	if (!(squash & BXT_DE_PLL_PLL_ENABLE)) {
		DBGLOG("ngreen", "wrapProbeCDClockFrequency: BXT_DE_PLL_PLL_ENABLE (0x46070 bit31) was clear (0x%x), setting it", squash);
		NGreen::callback->writeReg32(BXT_DE_PLL_ENABLE, squash | BXT_DE_PLL_PLL_ENABLE);
	}

	// Sonoma also panics if cdclk is not 0x50E (648 MHz) or 0x518 (652.8 MHz).
	// Frequencies 0x264, 0x26E, 0x44E, 0x458 trigger "Wrong CD clock frequency set by EFI".
	// sanitizeCDClockFrequency raises cdclk to 648/652.8 MHz to satisfy the original.
	auto cdclk = NGreen::callback->readReg32(ICL_REG_CDCLK_CTL) & CDCLK_FREQ_DECIMAL_MASK;  // byte addr OK
	if (cdclk < ICL_CDCLK_DEC_FREQ_THRESHOLD) {
		DBGLOG("ngreen", "wrapProbeCDClockFrequency: cdclk 0x%x below threshold, sanitizing", cdclk);
		sanitizeCDClockFrequency(that);
	}

	auto retVal = callback->orgProbeCDClockFrequency(that);
	return retVal;
}
/*uint32_t Gen11::wrapProbeCDClockFrequency(void *that) {

	//auto cdclk = callback->wrapReadRegister32(that, ICL_REG_CDCLK_CTL) & CDCLK_FREQ_DECIMAL_MASK;
	auto cdclk =NGreen::callback->readReg32(ICL_REG_CDCLK_CTL) & CDCLK_FREQ_DECIMAL_MASK;
	
	if (cdclk < ICL_CDCLK_DEC_FREQ_THRESHOLD) {
		sanitizeCDClockFrequency(that);
	}
	
	auto retVal = callback->orgProbeCDClockFrequency(that);
	return retVal;
}*/


bool Gen11::start(void *that,void  *param_1)
{
	// Inject MultiForceWakeSelect=1 into Development dictionary.
	// This tells the accelerator to use SafeForceWakeMultithreaded (which we hook)
	// instead of the framebuffer's SafeForceWake (which fails on RPL-P with ACK=0).
	auto *service = static_cast<IOService *>(that);
	auto *devDict = OSDynamicCast(OSDictionary, service->getProperty("Development"));
	if (devDict) {
		auto *newDevDict = OSDictionary::withDictionary(devDict);
		if (newDevDict) {
			auto *num = OSNumber::withNumber(1ULL, 32);
			if (num) {
				newDevDict->setObject("MultiForceWakeSelect", num);
				num->release();
			}
			service->setProperty("Development", newDevDict);
			newDevDict->release();
			SYSLOG("ngreen", "Injected MultiForceWakeSelect=1 into Development dict");
		}
	} else {
		SYSLOG("ngreen", "No Development dict found, creating one with MultiForceWakeSelect=1");
		auto *newDevDict = OSDictionary::withCapacity(4);
		if (newDevDict) {
			auto *num = OSNumber::withNumber(1ULL, 32);
			if (num) {
				newDevDict->setObject("MultiForceWakeSelect", num);
				num->release();
			}
			service->setProperty("Development", newDevDict);
			newDevDict->release();
		}
	}

	// ── V29: Apply GT workarounds + GGTT PTE diagnostics ──
	SYSLOG("ngreen", "Pre-start: acquiring ForceWake for GT workarounds");
	NGreen::callback->writeReg32(FORCEWAKE_RENDER_GEN9, (1 << 16) | 1);
	IODelay(1000);
	
	uint32_t fwAck = 0;
	uint64_t fwNow = 0, fwDeadline = 0;
	clock_interval_to_deadline(50, kMillisecondScale, &fwDeadline);
	for (clock_get_uptime(&fwNow); fwNow < fwDeadline; clock_get_uptime(&fwNow)) {
		fwAck = NGreen::callback->readReg32(FORCEWAKE_ACK_RENDER_GEN9);
		if (fwAck & 1) break;
	}
	SYSLOG("ngreen", "Pre-start ForceWake ACK: 0x%x %s", fwAck, (fwAck & 1) ? "OK" : "TIMEOUT");
	
	// Register defines
	#define ERROR_GEN6         0x40A0
	#define GEN12_RING_FAULT_REG 0xCEC4
	#define GEN8_FAULT_TLB_DATA0 0x4B10
	#define GEN8_FAULT_TLB_DATA1 0x4B14
	#define RING_EIR(base)     ((base) + 0xB0)
	#define RING_EMR(base)     ((base) + 0xB4)
	#define RING_ESR(base)     ((base) + 0xB8)
	#define RING_FAULT_REG(base) ((base) + 0x150)
	#define GEN11_GT_INTR_DW0  0x190018
	#define GEN11_GT_INTR_DW1  0x19001C
	#define RING_ACTHD(base)   ((base) + 0x74)
	#define RING_ACTHD_UDW(base) ((base) + 0x5C)
	#define RING_IPEHR(base)   ((base) + 0x68)
	#define RING_IPEIR(base)   ((base) + 0x64)
	#define RING_INSTDONE(base) ((base) + 0x6C)
	#define RING_DMA_FADD(base) ((base) + 0x78)
	#define RING_DMA_FADD_UDW(base) ((base) + 0x60)
	#define RING_INSTPM(base)  ((base) + 0xC0)
	#define RING_EXECLIST_STATUS(base) ((base) + 0x234)
	#define RING_CONTEXT_STATUS_PTR(base) ((base) + 0x3A0)
	#define RING_CONTEXT_STATUS_BUF(base, idx)    ((base) + 0x370 + (idx) * 8)
	#define RING_CONTEXT_STATUS_BUF_HI(base, idx) ((base) + 0x374 + (idx) * 8)
	// GGTT PTE base within BAR0 (Gen8+: 8MB into MMIO BAR, each PTE is 8 bytes)
	#define GEN8_GGTT_PTE_BASE 0x800000
	#define GGTT_PTE_LO(page)  (GEN8_GGTT_PTE_BASE + (page) * 8)
	#define GGTT_PTE_HI(page)  (GEN8_GGTT_PTE_BASE + (page) * 8 + 4)
	// Context size register
	#define RING_CTX_SIZE(base) ((base) + 0x1A0)
	// Current context ID
	#define RING_CCID(base)    ((base) + 0x180)
	// Context control
	#define RING_CTX_CTRL(base) ((base) + 0x244)
	// GFX mode (ring mode register)
	#define RING_MI_MODE(base) ((base) + 0x9C)
	#define RING_MODE(base)    ((base) + 0x29C)
	
	// ── V29 NEW: Pre-start GGTT PTE dump ──
	// Dump first few GGTT PTEs to verify format
	SYSLOG("ngreen", "GGTT PTE format check (first 4 pages):");
	for (int i = 0; i < 4; i++) {
		SYSLOG("ngreen", "  GGTT[%d]=0x%08x:%08x", i,
			NGreen::callback->readReg32(GGTT_PTE_HI(i)),
			NGreen::callback->readReg32(GGTT_PTE_LO(i)));
	}
	// Dump PTEs around the HWS page area (GGTT offset 0x40004000 → page 0x40004)
	// But that PTE would be at 0x800000 + 0x40004*8 = 0xA00020 — check if within BAR
	// Instead, dump some PTEs in the driver's active range
	SYSLOG("ngreen", "GGTT PTE near page 0x100:");
	for (int i = 0x100; i < 0x104; i++) {
		SYSLOG("ngreen", "  GGTT[0x%x]=0x%08x:%08x", i,
			NGreen::callback->readReg32(GGTT_PTE_HI(i)),
			NGreen::callback->readReg32(GGTT_PTE_LO(i)));
	}
	
	// ── V29 NEW: Context and engine config ──
	SYSLOG("ngreen", "RCS CTX_SIZE=0x%x CCID=0x%x CTX_CTRL=0x%x",
		NGreen::callback->readReg32(RING_CTX_SIZE(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_CCID(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_CTX_CTRL(RENDER_RING_BASE)));
	SYSLOG("ngreen", "RCS MI_MODE=0x%x RING_MODE=0x%x",
		NGreen::callback->readReg32(RING_MI_MODE(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_MODE(RENDER_RING_BASE)));
	
	SYSLOG("ngreen", "Pre-start ERROR_GEN6=0x%x", NGreen::callback->readReg32(ERROR_GEN6));
	
	// GT workarounds
	uint32_t misccpctl = NGreen::callback->readReg32(GEN7_MISCCPCTL);
	misccpctl &= ~GEN12_DOP_CLOCK_GATE_RENDER_ENABLE;
	NGreen::callback->writeReg32(GEN7_MISCCPCTL, misccpctl);
	NGreen::callback->wa_write_or(VDBOX_CGCTL3F10(RENDER_RING_BASE), IECPUNIT_CLKGATE_DIS);
	NGreen::callback->wa_write_or(VDBOX_CGCTL3F10(BLT_RING_BASE), IECPUNIT_CLKGATE_DIS);
	NGreen::callback->wa_write_or(VDBOX_CGCTL3F10(GEN11_VEBOX_RING_BASE), IECPUNIT_CLKGATE_DIS);
	NGreen::callback->wa_mcr_write_or(GEN10_DFR_RATIO_EN_AND_CHICKEN, DFR_DISABLE);
	NGreen::callback->wa_masked_en(GEN11_COMMON_SLICE_CHICKEN3,
			GEN12_DISABLE_CPS_AWARE_COLOR_PIPE);
	NGreen::callback->wa_masked_field_set(GEN8_CS_CHICKEN1,
				GEN9_PREEMPT_GPGPU_LEVEL_MASK,
				GEN9_PREEMPT_GPGPU_THREAD_GROUP_LEVEL);
	uint32_t mcr = GEN8_MCR_SLICE(0) | GEN8_MCR_SUBSLICE(0);
	uint32_t mcr_mask = GEN8_MCR_SLICE_MASK | GEN8_MCR_SUBSLICE_MASK;
	NGreen::callback->wa_write_clr_set(GEN8_MCR_SELECTOR, mcr_mask, mcr);
	NGreen::callback->wa_masked_en(GEN7_FF_SLICE_CS_CHICKEN1,
			GEN9_FFSC_PERCTX_PREEMPT_CTRL);
	
	SYSLOG("ngreen", "Pre-start: GT workarounds applied, calling original start()");
	
	// Release Render ForceWake
	NGreen::callback->writeReg32(FORCEWAKE_RENDER_GEN9, (1 << 16) | 0);
	
	auto ret= FunctionCast(start, callback->ostart)(that,param_1);
	
	// ── V29: Post-start diagnostics ──
	NGreen::callback->writeReg32(FORCEWAKE_RENDER_GEN9, (1 << 16) | 1);
	NGreen::callback->writeReg32(FORCEWAKE_BLITTER_GEN9, (1 << 16) | 1);
	IODelay(1000);
	
	SYSLOG("ngreen", "start() returned %d", ret);
	
	// Ring buffer state
	uint32_t rcsHead = NGreen::callback->readReg32(RING_HEAD(RENDER_RING_BASE));
	uint32_t rcsTail = NGreen::callback->readReg32(RING_TAIL(RENDER_RING_BASE));
	uint32_t rcsCtl  = NGreen::callback->readReg32(RING_CTL(RENDER_RING_BASE));
	uint32_t rcsStart = NGreen::callback->readReg32(RING_START(RENDER_RING_BASE));
	uint32_t rcsHws  = NGreen::callback->readReg32(RING_HWS_PGA(RENDER_RING_BASE));
	SYSLOG("ngreen", "RCS0 HEAD=0x%x TAIL=0x%x CTL=0x%x START=0x%x",
		rcsHead, rcsTail, rcsCtl, rcsStart);
	SYSLOG("ngreen", "RCS0 HWS_PGA=0x%x HWSTAM=0x%x",
		rcsHws, NGreen::callback->readReg32(RING_HWSTAM(RENDER_RING_BASE)));
	
	SYSLOG("ngreen", "RCS0 ACTHD=0x%x:%08x IPEHR=0x%x IPEIR=0x%x",
		NGreen::callback->readReg32(RING_ACTHD_UDW(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_ACTHD(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_IPEHR(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_IPEIR(RENDER_RING_BASE)));
	SYSLOG("ngreen", "RCS0 INSTDONE=0x%x INSTPM=0x%x DMA_FADD=0x%x:%08x",
		NGreen::callback->readReg32(RING_INSTDONE(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_INSTPM(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_DMA_FADD_UDW(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_DMA_FADD(RENDER_RING_BASE)));
	SYSLOG("ngreen", "RCS0 EXECLIST_STATUS=0x%x CTX_STATUS_PTR=0x%x",
		NGreen::callback->readReg32(RING_EXECLIST_STATUS(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_CONTEXT_STATUS_PTR(RENDER_RING_BASE)));
	
	// Per-engine error
	SYSLOG("ngreen", "RCS0 EIR=0x%x ESR=0x%x EMR=0x%x",
		NGreen::callback->readReg32(RING_EIR(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_ESR(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_EMR(RENDER_RING_BASE)));
	SYSLOG("ngreen", "RCS0 RING_FAULT=0x%x",
		NGreen::callback->readReg32(RING_FAULT_REG(RENDER_RING_BASE)));
	
	// Global errors
	SYSLOG("ngreen", "ERROR_GEN6=0x%x RING_FAULT(global)=0x%x",
		NGreen::callback->readReg32(ERROR_GEN6),
		NGreen::callback->readReg32(GEN12_RING_FAULT_REG));
	SYSLOG("ngreen", "FAULT_TLB_DATA0=0x%x FAULT_TLB_DATA1=0x%x",
		NGreen::callback->readReg32(GEN8_FAULT_TLB_DATA0),
		NGreen::callback->readReg32(GEN8_FAULT_TLB_DATA1));
	SYSLOG("ngreen", "GT_INTR_DW0=0x%x DW1=0x%x",
		NGreen::callback->readReg32(GEN11_GT_INTR_DW0),
		NGreen::callback->readReg32(GEN11_GT_INTR_DW1));
	
	// ── V29 NEW: Context and engine config after start ──
	SYSLOG("ngreen", "RCS CTX_SIZE=0x%x CCID=0x%x CTX_CTRL=0x%x",
		NGreen::callback->readReg32(RING_CTX_SIZE(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_CCID(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_CTX_CTRL(RENDER_RING_BASE)));
	SYSLOG("ngreen", "RCS MI_MODE=0x%x RING_MODE=0x%x",
		NGreen::callback->readReg32(RING_MI_MODE(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_MODE(RENDER_RING_BASE)));
	
	// ── V29 NEW: GGTT PTEs for HWS page and ring buffer ──
	// HWS_PGA is a GGTT address; read its PTE to check format
	if (rcsHws) {
		uint32_t hwsPage = rcsHws >> 12;
		SYSLOG("ngreen", "GGTT PTE for HWS (page 0x%x)=0x%08x:%08x", hwsPage,
			NGreen::callback->readReg32(GGTT_PTE_HI(hwsPage)),
			NGreen::callback->readReg32(GGTT_PTE_LO(hwsPage)));
		// Also check adjacent pages
		SYSLOG("ngreen", "GGTT PTE for HWS+1 (page 0x%x)=0x%08x:%08x", hwsPage+1,
			NGreen::callback->readReg32(GGTT_PTE_HI(hwsPage+1)),
			NGreen::callback->readReg32(GGTT_PTE_LO(hwsPage+1)));
	}
	if (rcsStart) {
		uint32_t ringPage = rcsStart >> 12;
		SYSLOG("ngreen", "GGTT PTE for RING (page 0x%x)=0x%08x:%08x", ringPage,
			NGreen::callback->readReg32(GGTT_PTE_HI(ringPage)),
			NGreen::callback->readReg32(GGTT_PTE_LO(ringPage)));
	}
	
	// CSB entries
	uint32_t csp = NGreen::callback->readReg32(RING_CONTEXT_STATUS_PTR(RENDER_RING_BASE));
	SYSLOG("ngreen", "CSB wr_ptr=%d rd_ptr=%d", (csp >> 8) & 0x7, csp & 0x7);
	for (int i = 0; i < 6; i++) {
		SYSLOG("ngreen", "CSB[%d]=0x%x:%08x", i,
			NGreen::callback->readReg32(RING_CONTEXT_STATUS_BUF_HI(RENDER_RING_BASE, i)),
			NGreen::callback->readReg32(RING_CONTEXT_STATUS_BUF(RENDER_RING_BASE, i)));
	}
	
	SYSLOG("ngreen", "ForceWake ACK: Render=0x%x Blitter=0x%x",
		NGreen::callback->readReg32(FORCEWAKE_ACK_RENDER_GEN9),
		NGreen::callback->readReg32(FORCEWAKE_ACK_BLITTER_GEN9));
	
	// BCS full state (now with Blitter ForceWake held!)
	uint32_t bcsHead = NGreen::callback->readReg32(RING_HEAD(BLT_RING_BASE));
	uint32_t bcsTail = NGreen::callback->readReg32(RING_TAIL(BLT_RING_BASE));
	uint32_t bcsCtl  = NGreen::callback->readReg32(RING_CTL(BLT_RING_BASE));
	uint32_t bcsStart = NGreen::callback->readReg32(RING_START(BLT_RING_BASE));
	SYSLOG("ngreen", "BCS HEAD=0x%x TAIL=0x%x CTL=0x%x START=0x%x",
		bcsHead, bcsTail, bcsCtl, bcsStart);
	SYSLOG("ngreen", "BCS HWS_PGA=0x%x MI_MODE=0x%x",
		NGreen::callback->readReg32(RING_HWS_PGA(BLT_RING_BASE)),
		NGreen::callback->readReg32(RING_MI_MODE(BLT_RING_BASE)));
	SYSLOG("ngreen", "BCS ACTHD=0x%x:%08x IPEHR=0x%x IPEIR=0x%x",
		NGreen::callback->readReg32(RING_ACTHD_UDW(BLT_RING_BASE)),
		NGreen::callback->readReg32(RING_ACTHD(BLT_RING_BASE)),
		NGreen::callback->readReg32(RING_IPEHR(BLT_RING_BASE)),
		NGreen::callback->readReg32(RING_IPEIR(BLT_RING_BASE)));
	SYSLOG("ngreen", "BCS INSTDONE=0x%x EIR=0x%x ESR=0x%x EMR=0x%x",
		NGreen::callback->readReg32(RING_INSTDONE(BLT_RING_BASE)),
		NGreen::callback->readReg32(RING_EIR(BLT_RING_BASE)),
		NGreen::callback->readReg32(RING_ESR(BLT_RING_BASE)),
		NGreen::callback->readReg32(RING_EMR(BLT_RING_BASE)));
	SYSLOG("ngreen", "BCS EXECLIST_STATUS=0x%x CTX_STATUS_PTR=0x%x",
		NGreen::callback->readReg32(RING_EXECLIST_STATUS(BLT_RING_BASE)),
		NGreen::callback->readReg32(RING_CONTEXT_STATUS_PTR(BLT_RING_BASE)));
	
	// V36: Dump interrupt enable/mask state to verify GT interrupts are configured
	SYSLOG("ngreen", "IRQ: GFX_MSTR_IRQ=0x%x DISPLAY_INT_CTL=0x%x",
		NGreen::callback->readReg32(GEN11_GFX_MSTR_IRQ),
		NGreen::callback->readReg32(GEN11_DISPLAY_INT_CTL));
	SYSLOG("ngreen", "IRQ: RENDER_COPY_INTR_EN=0x%x VCS_VECS_INTR_EN=0x%x",
		NGreen::callback->readReg32(GEN11_RENDER_COPY_INTR_ENABLE),
		NGreen::callback->readReg32(GEN11_VCS_VECS_INTR_ENABLE));
	SYSLOG("ngreen", "IRQ: RCS0_RSVD_MASK=0x%x BCS_RSVD_MASK=0x%x",
		NGreen::callback->readReg32(GEN11_RCS0_RSVD_INTR_MASK),
		NGreen::callback->readReg32(GEN11_BCS_RSVD_INTR_MASK));
	
	// Release both ForceWake domains
	NGreen::callback->writeReg32(FORCEWAKE_RENDER_GEN9, (1 << 16) | 0);
	NGreen::callback->writeReg32(FORCEWAKE_BLITTER_GEN9, (1 << 16) | 0);
	
	return ret;
}
int Gen11::wrapPmNotifyWrapper(unsigned int a0, unsigned int a1, unsigned long long *a2, unsigned int *freq) {
	
	/*struct intel_rps_freq_caps *caps;
	
	caps->rp0_freq *= GEN9_FREQ_SCALER;
	caps->rp1_freq *= GEN9_FREQ_SCALER;
	caps->min_freq *= GEN9_FREQ_SCALER;
	
	uint32_t mult=GEN9_FREQ_SCALER;
	uint32_t ddcc_status = 0;
	
	*freq =caps->rp1_freq;
	return 0;*/
	
	uint32_t cfreq = 0;

	FunctionCast(wrapPmNotifyWrapper, callback->orgPmNotifyWrapper)(a0, a1, a2, &cfreq);
	
	if (!callback->freq_max) {
		callback->freq_max = wrapReadRegister32(callback->framecont, GEN6_RP_STATE_CAP) & 0xFF;

	}
	
	*freq = (GEN9_FREQ_SCALER << GEN9_FREQUENCY_SHIFT) * callback->freq_max;
	return 0;
}

bool Gen11::patchRCSCheck(mach_vm_address_t& start) {
	constexpr unsigned ninsts_max {256};
	
	hde64s dis;
	
	bool found_cmp = false;
	bool found_jmp = false;

	for (size_t i = 0; i < ninsts_max; i++) {
		auto sz = Disassembler::hdeDisasm(start, &dis);

		if (dis.flags & F_ERROR) {
			break;
		}

		/* cmp byte ptr [rcx], 0 */
		if (!found_cmp && dis.opcode == 0x80 && dis.modrm_reg == 7 && dis.modrm_rm == 1)
			found_cmp = true;
		/* jnz rel32 */
		if (found_cmp && dis.opcode == 0x0f && dis.opcode2 == 0x85) {
			found_jmp = true;
			break;
		}

		start += sz;
	}
	
	if (found_jmp) {
		auto status = MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock);
		if (status == KERN_SUCCESS) {
			constexpr uint8_t nop6[] {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
			lilu_os_memcpy(reinterpret_cast<void*>(start), nop6, arrsize(nop6));
			MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
			return true;
		} else {
			return false;
		}
	} else {
		return false;
	}
}

/**
 * Port of i915 force wake for Gen12 (TGL/ADL/RPL).
 * Replaces IntelAccelerator::SafeForceWakeMultithreaded.
 *
 * Differences from Apple's code:
 * 1. 50 ms ACK timeouts (Apple uses 90 ms) — https://patchwork.kernel.org/patch/7057561/
 * 2. Reserve-bit fallback on primary ACK timeout — https://patchwork.kernel.org/patch/10029821/
 * 3. Correct Gen9-style 3-domain iteration matching Apple's dom bitmask
 *    (dom: bit0=Render, bit1=Media, bit2=Blitter/GT)
 *
 * NOTE: The header's regForDom/ackForDom use Gen11+ domain bitmask IDs
 * (FORCEWAKE_RENDER=1=Render, FORCEWAKE_GT=2, FORCEWAKE_MEDIA=4) which does NOT
 * match Apple's 3-bit dom (1=Render, 2=Media, 4=Blitter). We use direct register
 * mapping here to match Apple's convention, same as WEG's ForceWakeWorkaround.
 */

// Map Apple's 3-bit domain to MMIO request register (Render + GT/Blitter same Gen9-Gen12)
static uint32_t fwReqReg(unsigned d) {
	if (d == DOM_RENDER)  return FORCEWAKE_RENDER_GEN9;   // 0xa278
	if (d == DOM_BLITTER) return FORCEWAKE_BLITTER_GEN9;  // 0xa188
	return 0;  // Media uses Gen11+ per-engine registers, handled separately
}

// Map Apple's 3-bit domain to MMIO ACK register (Render + GT/Blitter same Gen9-Gen12)
static uint32_t fwAckReg(unsigned d) {
	if (d == DOM_RENDER)  return FORCEWAKE_ACK_RENDER_GEN9;   // 0x0D84
	if (d == DOM_BLITTER) return FORCEWAKE_ACK_BLITTER_GEN9;  // 0x130044
	return 0;
}

// Gen12 Media ForceWake: per-engine register pairs (VDBOX + VEBOX)
struct FwMediaEngine {
	uint32_t req;
	uint32_t ack;
	const char *name;
};
static const FwMediaEngine fwMediaEngines[] = {
	{ FORCEWAKE_MEDIA_VDBOX_GEN11(0), FORCEWAKE_ACK_MEDIA_VDBOX_GEN11(0), "VDBOX0" },  // 0xa540 / 0x0D50
	{ FORCEWAKE_MEDIA_VEBOX_GEN11(0), FORCEWAKE_ACK_MEDIA_VEBOX_GEN11(0), "VEBOX0" },  // 0xa560 / 0x0D70
};

void Gen11::wrapSafeForceWake(void *that, bool set, uint32_t dom) {
	forceWake(that, set, dom, 1);  // forward to our forceWake with ctx=1 (normal, non-IRQ)
}

void Gen11::forceWake(void *that, bool set, uint32_t dom, uint8_t ctx) {
	SYSLOG("ngreen", "forceWake called: set=%d dom=0x%x ctx=%d", set, dom, ctx);
	
	// ── Hangcheck: dump GPU state once after stamp-timeout restart ──
	// During init, ~14 forceWake calls happen. After IOAcceleratorFamily2 submits
	// work and the stamp times out (~5s later), a burst of restart-related calls
	// occurs. Dump full RCS/BCS state once on the 30th call to capture post-hang state.
	static int fwCallCount = 0;
	static bool hangcheckDumped = false;
	fwCallCount++;
	
	if (!hangcheckDumped && fwCallCount == 30) {
		hangcheckDumped = true;
		SYSLOG("ngreen", "=== HANGCHECK: GPU state dump (fwCall=%d) ===", fwCallCount);
		
		// Acquire both Render + Blitter ForceWake for reliable reads
		NGreen::callback->writeReg32(FORCEWAKE_RENDER_GEN9, (1 << 16) | 1);
		NGreen::callback->writeReg32(FORCEWAKE_BLITTER_GEN9, (1 << 16) | 1);
		IODelay(1000);
		
		SYSLOG("ngreen", "HANGCHECK ForceWake ACK: Render=0x%x Blitter=0x%x",
			NGreen::callback->readReg32(FORCEWAKE_ACK_RENDER_GEN9),
			NGreen::callback->readReg32(FORCEWAKE_ACK_BLITTER_GEN9));
		
		// RCS ring state
		SYSLOG("ngreen", "HANGCHECK RCS HEAD=0x%x TAIL=0x%x CTL=0x%x START=0x%x",
			NGreen::callback->readReg32(RING_HEAD(RENDER_RING_BASE)),
			NGreen::callback->readReg32(RING_TAIL(RENDER_RING_BASE)),
			NGreen::callback->readReg32(RING_CTL(RENDER_RING_BASE)),
			NGreen::callback->readReg32(RING_START(RENDER_RING_BASE)));
		SYSLOG("ngreen", "HANGCHECK RCS ACTHD=0x%x:%08x IPEHR=0x%x",
			NGreen::callback->readReg32(RING_ACTHD_UDW(RENDER_RING_BASE)),
			NGreen::callback->readReg32(RING_ACTHD(RENDER_RING_BASE)),
			NGreen::callback->readReg32(RING_IPEHR(RENDER_RING_BASE)));
		SYSLOG("ngreen", "HANGCHECK RCS INSTDONE=0x%x DMA_FADD=0x%x:%08x",
			NGreen::callback->readReg32(RING_INSTDONE(RENDER_RING_BASE)),
			NGreen::callback->readReg32(RING_DMA_FADD_UDW(RENDER_RING_BASE)),
			NGreen::callback->readReg32(RING_DMA_FADD(RENDER_RING_BASE)));
		SYSLOG("ngreen", "HANGCHECK RCS EIR=0x%x ESR=0x%x EMR=0x%x",
			NGreen::callback->readReg32(RING_EIR(RENDER_RING_BASE)),
			NGreen::callback->readReg32(RING_ESR(RENDER_RING_BASE)),
			NGreen::callback->readReg32(RING_EMR(RENDER_RING_BASE)));
		SYSLOG("ngreen", "HANGCHECK RCS EXECLIST_STATUS=0x%x CTX_STATUS_PTR=0x%x",
			NGreen::callback->readReg32(RING_EXECLIST_STATUS(RENDER_RING_BASE)),
			NGreen::callback->readReg32(RING_CONTEXT_STATUS_PTR(RENDER_RING_BASE)));
		
		// BCS ring state (with proper Blitter ForceWake held!)
		SYSLOG("ngreen", "HANGCHECK BCS HEAD=0x%x TAIL=0x%x CTL=0x%x START=0x%x",
			NGreen::callback->readReg32(RING_HEAD(BLT_RING_BASE)),
			NGreen::callback->readReg32(RING_TAIL(BLT_RING_BASE)),
			NGreen::callback->readReg32(RING_CTL(BLT_RING_BASE)),
			NGreen::callback->readReg32(RING_START(BLT_RING_BASE)));
		SYSLOG("ngreen", "HANGCHECK BCS HWS_PGA=0x%x ACTHD=0x%x:%08x",
			NGreen::callback->readReg32(RING_HWS_PGA(BLT_RING_BASE)),
			NGreen::callback->readReg32(RING_ACTHD_UDW(BLT_RING_BASE)),
			NGreen::callback->readReg32(RING_ACTHD(BLT_RING_BASE)));
		SYSLOG("ngreen", "HANGCHECK BCS IPEHR=0x%x INSTDONE=0x%x",
			NGreen::callback->readReg32(RING_IPEHR(BLT_RING_BASE)),
			NGreen::callback->readReg32(RING_INSTDONE(BLT_RING_BASE)));
		SYSLOG("ngreen", "HANGCHECK BCS EIR=0x%x ESR=0x%x EMR=0x%x",
			NGreen::callback->readReg32(RING_EIR(BLT_RING_BASE)),
			NGreen::callback->readReg32(RING_ESR(BLT_RING_BASE)),
			NGreen::callback->readReg32(RING_EMR(BLT_RING_BASE)));
		
		// Global error state
		SYSLOG("ngreen", "HANGCHECK ERROR_GEN6=0x%x RING_FAULT=0x%x",
			NGreen::callback->readReg32(0x40A0),
			NGreen::callback->readReg32(0xCEC4));
		SYSLOG("ngreen", "HANGCHECK FAULT_TLB0=0x%x TLB1=0x%x",
			NGreen::callback->readReg32(0x4B10),
			NGreen::callback->readReg32(0x4B14));
		SYSLOG("ngreen", "HANGCHECK GT_INTR_DW0=0x%x DW1=0x%x",
			NGreen::callback->readReg32(0x190018),
			NGreen::callback->readReg32(0x19001C));
		
		// Release ForceWake
		NGreen::callback->writeReg32(FORCEWAKE_RENDER_GEN9, (1 << 16) | 0);
		NGreen::callback->writeReg32(FORCEWAKE_BLITTER_GEN9, (1 << 16) | 0);
		
		SYSLOG("ngreen", "=== HANGCHECK: dump complete ===");
	}
	
	// ctx 2: IRQ, ctx 1: normal
	uint32_t ack_exp = set << ctx;
	uint32_t mask = 1 << ctx;
	uint32_t wr = ack_exp | (1 << ctx << 16);

	for (unsigned d = DOM_FIRST; d <= DOM_LAST; d <<= 1) {
		if (!(dom & d)) continue;

		if (d == DOM_MEDIA) {
			// Gen12+: Media uses per-engine ForceWake (VDBOX + VEBOX), NOT Gen9 single register
			for (const auto &eng : fwMediaEngines) {
				wrapWriteRegister32(callback->framecont, eng.req, wr);
				IOPause(100);
				if (!pollRegister(eng.ack, ack_exp, mask, FORCEWAKE_ACK_TIMEOUT_MS) &&
					!forceWakeWaitAckFallback(eng.req, eng.ack, ack_exp, mask) &&
					!pollRegister(eng.ack, ack_exp, mask, FORCEWAKE_ACK_TIMEOUT_MS))
					SYSLOG("ngreen", "ForceWake timeout for %s (dom=0x%x), expected 0x%x", eng.name, dom, ack_exp);
			}
		} else {
			wrapWriteRegister32(callback->framecont, fwReqReg(d), wr);
			IOPause(100);
			if (!pollRegister(fwAckReg(d), ack_exp, mask, FORCEWAKE_ACK_TIMEOUT_MS) &&
				!forceWakeWaitAckFallback(fwReqReg(d), fwAckReg(d), ack_exp, mask) &&
				!pollRegister(fwAckReg(d), ack_exp, mask, FORCEWAKE_ACK_TIMEOUT_MS))
				SYSLOG("ngreen", "ForceWake timeout for domain %s (dom=0x%x), expected 0x%x", strForDom(d), dom, ack_exp);
		}
	}
}

bool Gen11::pollRegister(uint32_t reg, uint32_t val, uint32_t mask, uint32_t timeout) {
    uint64_t now = 0, deadline = 0;
    clock_interval_to_deadline(timeout, kMillisecondScale, &deadline);
    for (clock_get_uptime(&now); now < deadline; clock_get_uptime(&now)) {
        auto rd = wrapReadRegister32(callback->framecont, reg);
        if ((rd & mask) == val)
            return true;
    }
    return false;
}

bool Gen11::forceWakeWaitAckFallback(uint32_t reqReg, uint32_t ackReg, uint32_t val, uint32_t mask) {
	unsigned pass = 1;
	bool ack = false;
	auto controller = callback->framecont;
	
	do {
		pollRegister(ackReg, 0, FORCEWAKE_KERNEL_FALLBACK, FORCEWAKE_ACK_TIMEOUT_MS);
		wrapWriteRegister32(controller, reqReg, fw_set(FORCEWAKE_KERNEL_FALLBACK));
		
		IODelay(10 * pass);
		pollRegister(ackReg, FORCEWAKE_KERNEL_FALLBACK, FORCEWAKE_KERNEL_FALLBACK, FORCEWAKE_ACK_TIMEOUT_MS);

		ack = (wrapReadRegister32(controller, ackReg) & mask) == val;

		wrapWriteRegister32(controller, reqReg, fw_clear(FORCEWAKE_KERNEL_FALLBACK));
	} while (!ack && pass++ < 10);
	
	return ack;
}

void Gen11::releaseDoorbell()
{
	
	
}

bool Gen11::dotrue()
{
	
	return true;
}

int iniin=1;
void  Gen11::readAndClearInterrupts(void *that,void *param_1)
{
	
	if (iniin){
		iniin=0;
		SYSLOG("ngreen", "readAndClearInterrupts: first call — initializing Gen11 GT interrupts");
		
		wrapWriteRegister32(callback->framecont, GEN11_GFX_MSTR_IRQ, 0);
		wrapWriteRegister32(callback->framecont,GEN11_DISPLAY_INT_CTL, 0);
		uint32_t master_ctl=wrapReadRegister32(callback->framecont,GEN11_GFX_MSTR_IRQ);
		
		//Disable RCS, BCS, VCS and VECS class engines.
		wrapWriteRegister32(callback->framecont, GEN11_RENDER_COPY_INTR_ENABLE, 0);
		wrapWriteRegister32(callback->framecont, GEN11_VCS_VECS_INTR_ENABLE,	  0);
		
		// Restore masks irqs on RCS, BCS, VCS and VECS engines.
		wrapWriteRegister32(callback->framecont, GEN11_RCS0_RSVD_INTR_MASK,	~0);
		wrapWriteRegister32(callback->framecont, GEN11_BCS_RSVD_INTR_MASK,	~0);
		wrapWriteRegister32(callback->framecont, GEN11_VCS0_VCS1_INTR_MASK,	~0);
		wrapWriteRegister32(callback->framecont, GEN11_VCS2_VCS3_INTR_MASK,	~0);
		wrapWriteRegister32(callback->framecont, GEN11_VECS0_VECS1_INTR_MASK,	~0);
		
		wrapWriteRegister32(callback->framecont, GEN11_GPM_WGBOXPERF_INTR_ENABLE, 0);
		wrapWriteRegister32(callback->framecont, GEN11_GPM_WGBOXPERF_INTR_MASK,  ~0);
		wrapWriteRegister32(callback->framecont, GEN11_GUC_SG_INTR_ENABLE, 0);
		wrapWriteRegister32(callback->framecont, GEN11_GUC_SG_INTR_MASK,  ~0);
		
		
		
		uint32_t irqs = GT_RENDER_USER_INTERRUPT;
		uint32_t guc_mask = /*intel_uc_wants_guc(&gt->uc) ? GUC_INTR_GUC2HOST :*/ 0;
		uint32_t gsc_mask = 0;
		uint32_t heci_mask = 0;
		uint32_t dmask;
		uint32_t smask;
		
		irqs |= GT_CS_MASTER_ERROR_INTERRUPT |
		GT_CONTEXT_SWITCH_INTERRUPT |
		GT_WAIT_SEMAPHORE_INTERRUPT;
		
		dmask = irqs << 16 | irqs;
		smask = irqs << 16;
		
		
		/* Enable RCS, BCS, VCS and VECS class interrupts. */
		wrapWriteRegister32(callback->framecont, GEN11_RENDER_COPY_INTR_ENABLE, dmask);
		wrapWriteRegister32(callback->framecont, GEN11_VCS_VECS_INTR_ENABLE, dmask);
		
		/* Unmask irqs on RCS, BCS, VCS and VECS engines. */
		wrapWriteRegister32(callback->framecont, GEN11_RCS0_RSVD_INTR_MASK, ~smask);
		wrapWriteRegister32(callback->framecont, GEN11_BCS_RSVD_INTR_MASK, ~smask);
		wrapWriteRegister32(callback->framecont, GEN11_VCS0_VCS1_INTR_MASK, ~dmask);
		wrapWriteRegister32(callback->framecont, GEN11_VCS2_VCS3_INTR_MASK, ~dmask);
		wrapWriteRegister32(callback->framecont, GEN11_VECS0_VECS1_INTR_MASK, ~dmask);
		
		/*
		 * RPS interrupts will get enabled/disabled on demand when RPS itself
		 * is enabled/disabled.
		 */
		
		
		wrapWriteRegister32(callback->framecont, GEN11_GPM_WGBOXPERF_INTR_ENABLE, 0);
		wrapWriteRegister32(callback->framecont, GEN11_GPM_WGBOXPERF_INTR_MASK,  ~0);
		
		/* Same thing for GuC interrupts */
		wrapWriteRegister32(callback->framecont, GEN11_GUC_SG_INTR_ENABLE, 0);
		wrapWriteRegister32(callback->framecont, GEN11_GUC_SG_INTR_MASK,  ~0);
		
		wrapWriteRegister32(callback->framecont,GEN11_DISPLAY_INT_CTL, GEN11_DISPLAY_IRQ_ENABLE);
		wrapWriteRegister32(callback->framecont, GEN11_GFX_MSTR_IRQ, GEN11_MASTER_IRQ);
	}
	
	
	FunctionCast(readAndClearInterrupts, callback->oreadAndClearInterrupts)(that,param_1);
}


void * Gen11::serviceInterrupts(void *param_1)
{
	return FunctionCast(serviceInterrupts, callback->oserviceInterrupts)(param_1);
	
}

void * Gen11::wprobe(void *that,void *param_1,int *param_2)
{
	//FunctionCast(wprobe, callback->owprobe)(that, param_1,param_2);
	//logStateInRegistry(that,0x56);
	initializeLogging(that);
	return that;
	
}

void *contr;
bool  Gen11::tgstart(void *that,void *param_1)
{
	contr=that;
	FunctionCast(tgstart, callback->otgstart)(that, param_1);
	return true;
	
}

void Gen11::FBMemMgr_Init(void *that)
{
	ccont = getMember<void *>(that, 0xc40);
	
	FunctionCast(FBMemMgr_Init, callback->oFBMemMgr_Init)(that);
	
	

	/*IODeviceMemory * m= NGreen::callback->iGPU->getDeviceMemoryWithIndex(0);
	IODeviceMemory *dm;
	m->withSubRange(dm,0x4180000,0x12000);//fDSBBufferBytes = 73728, fDSBBufferBaseOffset = 68681728
	IOMemoryMap *dsb=dm->map();
	
	IODeviceMemory *dm2;
	m->withSubRange(dm2,0x4192000,0x3000);//fConnectionStatusBytes = 12288, fConnectionStatusOffset = 68755456
	IOMemoryMap *dsb2=dm2->map();*/
	
}

uint32_t Gen11::probePortMode()
{
	auto ret=FunctionCast(probePortMode, callback->oprobePortMode)();
	return ret;
};


uint32_t Gen11::wdepthFromAttribute(void *that,uint param_1)
{
	return 0x1e;
};

uint32_t Gen11::raReadRegister32(void *that,unsigned long param_1)
{
	if (reinterpret_cast<volatile uint64_t*>(that)==nullptr) return NGreen::callback->readReg32(param_1);
	//PANIC_COND(reinterpret_cast<volatile uint64_t*>(that)==nullptr, "ngreen", "raReadRegister32 Failed 0x%lx",param_1);
	if (reinterpret_cast<volatile uint64_t*>(that)==nullptr) return 0;
	
	//if (param_1 >=0x2000 && NGreen::callback->readReg32(param_1)) return NGreen::callback->readReg32(param_1);
	auto ret=FunctionCast(raReadRegister32, callback->oraReadRegister32)(that,param_1);
	return ret;
};

unsigned long Gen11::raReadRegister32b(void *that,void *param_1,unsigned long param_2)
{
	//if (reinterpret_cast<volatile uint64_t*>(that)==nullptr) return 0;
	//if (reinterpret_cast<volatile uint64_t*>(param_1)==nullptr) return 0;
	return  raReadRegister32(that,reinterpret_cast<uint64_t>(param_1) + param_2);
};


uint64_t Gen11::raReadRegister64(void *that,unsigned long param_1)
{
	//if (reinterpret_cast<volatile uint64_t*>(that)==nullptr) return 0;
	
	return FunctionCast(raReadRegister64, callback->oraReadRegister64)(that,param_1);
};
uint64_t Gen11::raReadRegister64b(void *that,void *param_1,unsigned long param_2)
{
	//if (reinterpret_cast<volatile uint64_t*>(that)==nullptr) return 0;
	return  raReadRegister64(that,reinterpret_cast<uint64_t>(param_1) + param_2);
};

void Gen11::radWriteRegister32(void *that,unsigned long param_1, UInt32 param_2)
{
	radWriteRegister32f( that,param_1,param_2);
};

void Gen11::radWriteRegister32f(void *that,unsigned long param_1, UInt32 param_2)
{
	//FunctionCast(radWriteRegister32f, callback->oradWriteRegister32f)( that,param_1,param_2);
};

void Gen11::raWriteRegister32(void *that,unsigned long param_1, UInt32 param_2)
{
	// Force linear tiling for display plane registers.
	// Without GPU acceleration, surfaces are linear but the driver computes
	// Tile4 stride (÷512). Convert back to linear stride (÷64) = multiply by 8.
	// PLANE_CTL bits[14:12] = tiling mode: force 000 (linear).
	if (param_1 == 0x70188) { // PLANE_STRIDE Pipe A Plane 1
		UInt32 linear = param_2 * 8;
		DBGLOG("ngreen", "PLANE_STRIDE fixup: 0x%x -> 0x%x (linear)", param_2, linear);
		param_2 = linear;
	}
	if (param_1 == 0x70180) { // PLANE_CTL Pipe A Plane 1
		param_2 &= ~(0x7u << 12); // clear tiling bits → linear
		DBGLOG("ngreen", "PLANE_CTL fixup: forced linear tiling");
	}

	if (reinterpret_cast<volatile uint64_t*>(that)==nullptr) return NGreen::callback->writeReg32(param_1,param_2);
	if (!callback->oraWriteRegister32) return NGreen::callback->writeReg32(param_1,param_2);
	FunctionCast(raWriteRegister32, callback->oraWriteRegister32)( that,param_1,param_2);
};

void Gen11::raWriteRegister32f(void *that,unsigned long param_1, UInt32 param_2)
{
	FunctionCast(raWriteRegister32f, callback->oraWriteRegister32f)( that,param_1,param_2);
};

void Gen11::raWriteRegister32b(void *that,void *param_1,unsigned long param_2, UInt32 param_3)
{
	//if (reinterpret_cast<volatile uint64_t*>(that)==nullptr) return;
	//if (reinterpret_cast<volatile uint64_t*>(param_1)==nullptr) return;
	raWriteRegister32(that, reinterpret_cast<uint64_t>(param_1) + param_2,param_3);
};
void Gen11::raWriteRegister64(void *that,unsigned long param_1,UInt64 param_2)
{
	//if (reinterpret_cast<volatile uint64_t*>(that)==nullptr) return;
	FunctionCast(raWriteRegister64, callback->oraWriteRegister64)( that,param_1,param_2);
};

void Gen11::raWriteRegister64b(void *that,void *param_1,unsigned long param_2,UInt64 param_3)
{
	//if (reinterpret_cast<volatile uint64_t*>(that)==nullptr) return;
	raWriteRegister64( that,reinterpret_cast<uint64_t>(param_1) + param_2,param_3);
};

void Gen11::setupPlanarSurfaceDBUF()
{
	//FunctionCast(setupPlanarSurfaceDBUF, callback->osetupPlanarSurfaceDBUF)();
};

void Gen11::updateDBUF(void *that,uint param_1,uint param_2,bool param_3)
{
	//setupPlanarSurfaceDBUF();
};

int Gen11::LightUpEDP(void *that,void *param_1, void *param_2,void *param_3)
{
	FunctionCast(LightUpEDP, callback->oLightUpEDP)(that,param_1,param_2,param_3);
	return 0;
};


int Gen11::handleLinkIntegrityCheck()
{
	return 0;
};

uint8_t Gen11::disableVDDForAux(void *that)
{
	uint32_t iVar3 = raReadRegister32(ccont,0xc7200);
	if (-1 < iVar3) {
		IORegistryEntry *r= (IORegistryEntry *)getMember<long *>(that, 0xd60);
		r->setProperty("AAPL,LCD-PowerState-ON", false);
	}
	return FunctionCast(disableVDDForAux, callback->odisableVDDForAux)(that);
};

void  Gen11::prepareToEnterWake(void *that)
{
	
	bool Var5 = isPanelPowerOn(getMember<void *>(that, 0x1d0));
	if (Var5) {
		IORegistryEntry *r= (IORegistryEntry *)that;
		r->setProperty("AAPL,LCD-PowerState-ON", true);
	}
	FunctionCast(prepareToEnterWake, callback->oprepareToEnterWake)(that);
	
};

void Gen11::enableComboPhyEv(void *that)
{
	// RPL-P combo PHY is fully calibrated by firmware before macOS boots.
	// The TGL driver's enableComboPhyEv reads PORT_COMP_DW3 fuse registers and
	// validates vccIO / process fields against TGL-specific ranges. RPL-P silicon
	// fuse values don't match TGL expectations → panic at AppleIntelPortHAL.cpp:2405
	// ("Values for vccIO & process are invalid") via .cold.1.
	// V32 tried conditional skip (check OOB first) but register reads returned
	// misleading values before PHY power-up, letting the original run and panic.
	// V33: skip unconditionally — firmware calibration is sufficient for RPL-P.
	static const uint32_t dw3Regs[] = { 0x16210Cu, 0x06C10Cu };
	for (int i = 0; i < 2; i++) {
		uint32_t val = NGreen::callback->readReg32(dw3Regs[i]);
		uint32_t vccIO   = (val >> 24) & 0x3u;
		uint32_t process = (val >> 26) & 0x3u;
		SYSLOG("ngreen", "enableComboPhyEv: PHY%c DW3=0x%x vccIO=%u process=%u (skipped)",
		       'A' + i, val, vccIO, process);
	}
	SYSLOG("ngreen", "enableComboPhyEv: RPL-P — skipping original unconditionally (firmware calibrated)");
};

void Gen11::setPanelPowerState(void *that,bool param_1)
{
	FunctionCast(setPanelPowerState, callback->osetPanelPowerState)(that,param_1);
	
	IORegistryEntry *r= (IORegistryEntry *)getMember<long *>(that, 0xd60);
	r->setProperty("AAPL,LCD-PowerState-ON", param_1);
};


unsigned long Gen11::fastLinkTraining()
{
	
	FunctionCast(fastLinkTraining, callback->ofastLinkTraining)();
	return 1;
};

void Gen11::logStateInRegistry(void *that,uint param_1)
{
 FunctionCast(logStateInRegistry, callback->ologStateInRegistry)(that,param_1 );
}

void Gen11::initializeLogging(void *that)
{
	FunctionCast(initializeLogging, callback->oinitializeLogging)(that );
}

int Gen11::getPlatformID()
{
 return FunctionCast(getPlatformID, callback->ogetPlatformID)( );
}

uint32_t Gen11::tprobePortMode(void * that)
{
 return Genx::callback->tprobePortMode(that );
}

void  Gen11::AppleIntelPlanec1(void *that)
{
	Genx::callback->AppleIntelPlanec1(that );
}

void  Gen11::AppleIntelScalerc1(void *that)
{
	Genx::callback->AppleIntelScalerc1(that );
}


void * Gen11::AppleIntelScalernew(unsigned long param_1)
{
	return Genx::callback->AppleIntelScalernew(param_1 );
}
void * Gen11::AppleIntelPlanenew(unsigned long param_1)
{
	return Genx::callback->AppleIntelPlanenew(param_1 );
}

void Gen11::uupdateDBUF(void *that,uint param_1,uint param_2,bool param_3)
{
	Genx::callback->uupdateDBUF(that,param_1,param_2 );
}



bool inpwell=false;
void Gen11::PowerWellinit(void *that,void *param_1)
{
	inpwell=true;
  FunctionCast(PowerWellinit, callback->oPowerWellinit)(that,param_1 );
	inpwell=false;
}

long Gen11::getPortByDDI(uint param_1)
{
 auto ret= FunctionCast(getPortByDDI, callback->ogetPortByDDI)(param_1 );
	if (inpwell) setPortMode((void*)ret,1);
 return ret;
}

uint8_t  Gen11::setPortMode(void *that,uint32_t param_1)
{
 auto ret= FunctionCast(setPortMode, callback->osetPortMode)(that,param_1 );
	
 return ret;
}


IOReturn Gen11::wrapICLReadAUX(void *that, uint32_t address, void *buffer, uint32_t length) {

	IOReturn retVal =	FunctionCast(wrapICLReadAUX, callback->orgICLReadAUX)(that,address, buffer, length );

	if (address != 0x0000 && address != 0x2200)	return retVal;
	
	auto caps = reinterpret_cast<DPCDCap16*>(buffer);
	
	if (caps->revision < 0x03) {
		caps->maxLinkRate=0;
	}
	
	return retVal;
}

int smo=0;

int Gen11::isConflictRegister()
{
	
	return -1;

}

void Gen11::AppleIntelPowerWellinit(void *that,void *param_1)
{
	ccont = getMember<void *>(param_1, 0xc40);
	FunctionCast(AppleIntelPowerWellinit, callback->oAppleIntelPowerWellinit)(that,param_1);
}

bool Gen11::AppleIntelBaseControllerstart(void *that,void *param_1)
{
	// V25: Display workarounds BEFORE start (no ForceWake needed for display regs 0x4xxxx+).
	// GT workarounds moved AFTER start (ForceWake must be held for GT regs 0x0-0x7FFF).
	
	SYSLOG("ngreen", "AppleIntelBaseControllerstart: applying display workarounds");
	
	// Disable DC states during init to prevent power domain conflicts
	NGreen::callback->writeReg32(DC_STATE_EN, 0);
	
	/* Wa_14011294188:ehl,jsl,tgl,rkl,adl-s */
	NGreen::callback->intel_de_rmw(SOUTH_DSPCLK_GATE_D, 0,
				PCH_DPMGUNIT_CLOCK_GATE_DISABLE);
	
	// PCH reset handshake
	NGreen::callback->intel_de_rmw(HSW_NDE_RSTWRN_OPT, RESET_PCH_HANDSHAKE_ENABLE,
				RESET_PCH_HANDSHAKE_ENABLE);
	
	/* Wa_14011508470:tgl,dg1,rkl,adl-s,adl-p,dg2 */
	NGreen::callback->intel_de_rmw(GEN11_CHICKEN_DCPR_2, 0,
				DCPR_CLEAR_MEMSTAT_DIS | DCPR_SEND_RESP_IMM |
				DCPR_MASK_LPMODE | DCPR_MASK_MAXLATENCY_MEMUP_CLR);
	
	/* Display WA #1185 WaDisableDARBFClkGating:glk,icl,ehl,tgl (Wa_14010480278) */
	NGreen::callback->intel_de_rmw(GEN9_CLKGATE_DIS_0, 0, DARBF_GATING_DIS);
	
	/* Wa_14013723622 */
	NGreen::callback->intel_de_rmw(CLKREQ_POLICY, CLKREQ_POLICY_MEM_UP_OVRD, 0);
	
	SYSLOG("ngreen", "AppleIntelBaseControllerstart: display workarounds applied");
	
	SYSLOG("ngreen", "FBController::start() entering...");
	auto ret=FunctionCast(AppleIntelBaseControllerstart, callback->oAppleIntelBaseControllerstart)(that,param_1 );
	SYSLOG("ngreen", "FBController::start() returned %d", ret);
	
	if (ret) {
		// The original start succeeded but the accelerator (IntelAccelerator from TGL HW kext) never
		// gets matched because the com.xxxxx kext's IOKitPersonality is NOT in the IOCatalogue.
		// Lilu loaded the binary and we patched its code, but IOKit doesn't know the personality exists.
		// Fix: manually inject the personality into IOCatalogue so IOKit will match IntelAccelerator
		// against the PCI device under the IOAccelerator match category.
		
		auto *service = OSDynamicCast(IOService, reinterpret_cast<OSObject *>(that));
		if (service) {
			SYSLOG("ngreen", "FBController: injecting IntelAccelerator personality into IOCatalogue");
			
			auto *dict = OSDictionary::withCapacity(8);
			if (dict) {
				auto *bi  = OSString::withCString("com.xxxxx.driver.AppleIntelTGLGraphics");
				auto *cls = OSString::withCString("IntelAccelerator");
				auto *mc  = OSString::withCString("IOAccelerator");
				auto *pv  = OSString::withCString("IOPCIDevice");
				auto *pcm = OSString::withCString("0x03000000&0xff000000");
				auto *ps  = OSNumber::withNumber(static_cast<unsigned long long>(1000), 32);
				
				dict->setObject("CFBundleIdentifier", bi);
				dict->setObject("IOClass", cls);
				dict->setObject("IOMatchCategory", mc);
				dict->setObject("IOProviderClass", pv);
				dict->setObject("IOPCIClassMatch", pcm);
				dict->setObject("IOProbeScore", ps);
				
				OSSafeReleaseNULL(bi);
				OSSafeReleaseNULL(cls);
				OSSafeReleaseNULL(mc);
				OSSafeReleaseNULL(pv);
				OSSafeReleaseNULL(pcm);
				OSSafeReleaseNULL(ps);
				
				auto *array = OSArray::withCapacity(1);
				if (array) {
					array->setObject(dict);
					if (gIOCatalogue) {
						bool ok = gIOCatalogue->addDrivers(array, true);
						SYSLOG("ngreen", "FBController: addDrivers to gIOCatalogue returned %d", ok);
					} else {
						SYSLOG("ngreen", "FBController: gIOCatalogue is null!");
					}
					array->release();
				}
				dict->release();
			}
			
			SYSLOG("ngreen", "FBController: calling registerService() to trigger accelerator matching");
			service->registerService();
		}
	}
	
	return ret;
}


/*
void Gen11::initCDClock(void *that)
{
	// Mirrors AppleIntelFramebufferController::initCDClock decompiled flow.
	// DSSM bits [31:29] encode the reference clock frequency (0=24MHz, 1=19.2MHz, 2=38.4MHz).
	// The check < 0x60000000 ensures bits[31:29] < 3 (i.e. a valid reference).
	auto dssm = NGreen::callback->readReg32(ICL_REG_DSSM);
	if (dssm >= 0x60000000) {
		// Invalid / unsupported reference clock — fall back to original which will panic/reset.
		return FunctionCast(initCDClock, callback->oinitCDClock)(that);
	}

	uint32_t refFreqIdx = dssm >> 0x1d;                       // bits [31:29]
	getMember<uint32_t>(that, 0xe98) = refFreqIdx;

	// Call through our safe wrapper so BXT_DE_PLL_ENABLE / CDCLK sanity checks are applied.
	auto probedFreq = static_cast<uint64_t>(wrapProbeCDClockFrequency(that));
	getMember<uint64_t>(that, 0xe88) = probedFreq;

	if (probedFreq == 0) {
		// Fallback table — mirrors DAT_00081140[refFreqIdx * 0x24], first uint32 per entry.
		static const uint32_t kDefaultCDClkFreq[] = {
			ICL_CDCLK_FREQ_648_0,   // index 0: 24.0 MHz ref → 648 MHz
			ICL_CDCLK_FREQ_652_8,   // index 1: 19.2 MHz ref → 652.8 MHz
			ICL_CDCLK_FREQ_652_8,   // index 2: 38.4 MHz ref → 652.8 MHz
		};
		probedFreq = (refFreqIdx < arrsize(kDefaultCDClkFreq))
			? kDefaultCDClkFreq[refFreqIdx]
			: ICL_CDCLK_FREQ_648_0;
	}

	getMember<uint64_t>(that, 0xe90) = probedFreq;
}
*/
void Gen11::initCDClock(void *that)
{
	return FunctionCast(initCDClock, callback->oinitCDClock)(that);
}

void Gen11::setCDClockFrequencyOnHotplug(void *that)
{
	return FunctionCast(setCDClockFrequencyOnHotplug, callback->osetCDClockFrequencyOnHotplug)(that );
}


void Gen11::disableCDClock(void *that)
{
	FunctionCast(disableCDClock, callback->odisableCDClock)(that );
}


bool Gen11::AppleIntelFramebufferinit(void *frame,void *cont,uint32_t param_2)
{
	
	auto ret=FunctionCast(AppleIntelFramebufferinit, callback->oAppleIntelFramebufferinit)(frame,cont,param_2 );
	getMember<void *>(frame, 0x4a40) = ccont;
	return ret;
}

uint8_t  Gen11::AppleIntelPlaneinit(void *that,uint8_t param_1)
{
	return FunctionCast(AppleIntelPlaneinit, callback->oAppleIntelPlaneinit)(that,param_1 );
}

unsigned long Gen11::AppleIntelScalerinit(void *that,uint8_t param_1)
{
	return FunctionCast(AppleIntelScalerinit, callback->oAppleIntelScalerinit)(that,param_1 );
}

void  Gen11::disableScaler(void *that,bool param_1)
{
	getMember<void *>(that, 0x28) = ccont;
	FunctionCast(disableScaler, callback->odisableScaler)(that,param_1 );
}

void Gen11::programPipeScaler(void *that,void *param_1)
{
	getMember<void *>(that, 0x28) = ccont;
	FunctionCast(programPipeScaler, callback->oprogramPipeScaler)(that,param_1 );
}

void  Gen11::enablePlane(void *that,bool param_1)
{
	getMember<void *>(that, 0x90) = ccont;
	FunctionCast(enablePlane, callback->oenablePlane)(that,param_1 );
}

void Gen11::AppleIntelScalerupdateRegisterCache(void *that)
{
	getMember<void *>(that, 0x28) = ccont;
	FunctionCast(AppleIntelScalerupdateRegisterCache, callback->oAppleIntelScalerupdateRegisterCache)(that );
}

void Gen11::AppleIntelPlaneupdateRegisterCache(void *that)
{
	getMember<void *>(that, 0x90) = ccont;
	FunctionCast(AppleIntelPlaneupdateRegisterCache, callback->oAppleIntelPlaneupdateRegisterCache)(that );
}

void Gen11::enableDisplayEngine(void *that)
{
	getMember<void *>(that, 0x78) = ccont;
	FunctionCast(enableDisplayEngine, callback->oenableDisplayEngine)(that );
}

void Gen11::disableDisplayEngine(void *that)
{
	getMember<void *>(that, 0x78) = ccont;
	FunctionCast(disableDisplayEngine, callback->odisableDisplayEngine)(that );
}

void  Gen11::disablePowerWellPG(void *that,uint param_1)
{
	getMember<void *>(that, 0x78) = ccont;
	FunctionCast(disablePowerWellPG, callback->odisablePowerWellPG)(that,param_1 );
}
void  Gen11::enablePowerWellPG(void *that,uint param_1)
{
	getMember<void *>(that, 0x78) = ccont;
	FunctionCast(enablePowerWellPG, callback->oenablePowerWellPG)(that,param_1 );
}

void Gen11::hwSetPowerWellStatePG(void *that,bool param_1,uint param_2)
{
	getMember<void *>(that, 0x78) = ccont;
	FunctionCast(hwSetPowerWellStatePG, callback->ohwSetPowerWellStatePG)(that,param_1,param_2);
}

void Gen11::hwSetPowerWellStateDDI(void *that,bool param_1,uint param_2)
{
	getMember<void *>(that, 0x78) = ccont;
	FunctionCast(hwSetPowerWellStateDDI, callback->ohwSetPowerWellStateDDI)(that,param_1,param_2);
}

void Gen11::hwSetPowerWellStateAux(void *that,bool param_1,uint param_2)
{
	getMember<void *>(that, 0x78) = ccont;
	FunctionCast(hwSetPowerWellStateAux, callback->ohwSetPowerWellStateAux)(that,param_1,param_2);
}


void Gen11::hwInitializeCState(void *that)
{
	SYSLOG("ngreen", "NB-BUILD-V37-COMBOPHY-SKIP-UNCONDITIONAL");
	int origB48 = getMember<int>(that, 0xB48);
	int origCE4 = getMember<int>(that, 0xCE4);
	SYSLOG("ngreen", "hwInitCState B48=%d CE4=%d", origB48, origCE4);

	// Boot-arg "ngreen-dmc":
	//   not set or "skip" → safe fallback: passthrough original + AUX only (proven working)
	//   "tgl"             → load TGL DMC v2.12 blob + TGL display engine registers
	//   "adlp"            → load ADL-P DMC v2.16 blob + ADL-P display engine registers
	char dmcArg[16] = {};
	PE_parse_boot_argn("ngreen-dmc", dmcArg, sizeof(dmcArg));

	if (dmcArg[0] == 't' || dmcArg[0] == 'T') {
		// ── TGL DMC ──
		SYSLOG("ngreen", "hwInitCState: ngreen-dmc=tgl, loading TGL DMC v2.12 (%u bytes)", tgl_dmc_ver2_12_bin_s);
		getMember<int>(that, 0xB48) = 0; // suppress original CSR load
		FunctionCast(hwInitializeCState, callback->ohwInitializeCState)(that);
		getMember<int>(that, 0xB48) = origB48;
		// Write TGL DMC blob to MMIO 0x80000+
		for (unsigned long off = 0; off < tgl_dmc_ver2_12_bin_s; off += 4)
			FastWriteRegister32(ccont, off + 0x80000,
				*(const uint32_t *)((const char *)tgl_dmc_ver2_12_bin + off));
		// TGL display engine registers
		NGreen::callback->writeReg32(0x8F074, 0x00006FC0);
		NGreen::callback->writeReg32(0x8F004, 0x00A40088);
		NGreen::callback->writeReg32(0x8F034, 0xC003B400);
		// Enable DMC
		NGreen::callback->writeReg32(0x45520, 2);
		SYSLOG("ngreen", "hwInitCState: TGL DMC loaded");

	} else if (dmcArg[0] == 'a' || dmcArg[0] == 'A') {
		// ── ADL-P DMC ──
		SYSLOG("ngreen", "hwInitCState: ngreen-dmc=adlp, loading ADL-P DMC v2.16 (%u bytes)", adlp_dmc_ver2_16_bin_s);
		getMember<int>(that, 0xB48) = 0; // suppress original CSR load
		FunctionCast(hwInitializeCState, callback->ohwInitializeCState)(that);
		getMember<int>(that, 0xB48) = origB48;
		// Write ADL-P DMC blob to MMIO 0x80000+
		for (unsigned long off = 0; off < adlp_dmc_ver2_16_bin_s; off += 4)
			FastWriteRegister32(ccont, off + 0x80000,
				*(const uint32_t *)((const char *)adlp_dmc_ver2_16_bin + off));
		// ADL-P / RPL-P display engine registers
		// DDI A+B (Gen12 / Gen13 shared combo PHY)
		NGreen::callback->writeReg32(0x8F074, 0x00086FC0);
		NGreen::callback->writeReg32(0x8F034, 0xC003B400);
		NGreen::callback->writeReg32(0x8F004, 0x01240108);
		NGreen::callback->writeReg32(0x8F008, 0x512050D4);
		NGreen::callback->writeReg32(0x8F03C, 0xC003B300);
		NGreen::callback->writeReg32(0x8F00C, 0x584C57FC);
		// DDI C-F (ADL-P TC port registers)
		NGreen::callback->writeReg32(0x5F074, 0x00096FC0);
		NGreen::callback->writeReg32(0x5F034, 0xC003DF00);
		NGreen::callback->writeReg32(0x5F004, 0x214C2114);
		NGreen::callback->writeReg32(0x5F038, 0xC003E000);
		NGreen::callback->writeReg32(0x5F008, 0x22402208);
		NGreen::callback->writeReg32(0x5F03C, 0xC0032C00);
		NGreen::callback->writeReg32(0x5F00C, 0x241422FC);
		NGreen::callback->writeReg32(0x5F040, 0xC0033100);
		NGreen::callback->writeReg32(0x5F010, 0x26F826CC);
		NGreen::callback->writeReg32(0x5F474, 0x0009EFC0);
		NGreen::callback->writeReg32(0x5F434, 0xC003DF00);
		NGreen::callback->writeReg32(0x5F404, 0xA968A930);
		NGreen::callback->writeReg32(0x5F438, 0xC003E000);
		NGreen::callback->writeReg32(0x5F408, 0xAA5CAA24);
		NGreen::callback->writeReg32(0x5F43C, 0xC0032C00);
		NGreen::callback->writeReg32(0x5F40C, 0xAC30AB18);
		NGreen::callback->writeReg32(0x5F440, 0xC0033100);
		NGreen::callback->writeReg32(0x5F410, 0xAF14AEE8);
		NGreen::callback->writeReg32(0x5F874, 0x00053FC0);
		NGreen::callback->writeReg32(0x5F83C, 0xC0032C00);
		NGreen::callback->writeReg32(0x5F80C, 0x25202408);
		NGreen::callback->writeReg32(0x5F840, 0xC0033100);
		NGreen::callback->writeReg32(0x5F810, 0x280427D8);
		NGreen::callback->writeReg32(0x5FC3C, 0xC0032C00);
		NGreen::callback->writeReg32(0x5FC0C, 0x95209408);
		NGreen::callback->writeReg32(0x5FC40, 0xC0033100);
		NGreen::callback->writeReg32(0x5FC10, 0x980497D8);
		// CDClk / DPLL PLL coefficients
		NGreen::callback->writeReg32(0x60400, 0x8A000006);
		NGreen::callback->writeReg32(0x60000, 0x0A9F09FF);
		NGreen::callback->writeReg32(0x60004, 0x0A9F09FF);
		NGreen::callback->writeReg32(0x60008, 0x0A4F0A2F);
		NGreen::callback->writeReg32(0x6000C, 0x06D5063F);
		NGreen::callback->writeReg32(0x60010, 0x06D50000);
		NGreen::callback->writeReg32(0x60014, 0x06480642);
		NGreen::callback->writeReg32(0x60028, 0x00000000);
		NGreen::callback->writeReg32(0x60030, 0x7E5D159E);
		NGreen::callback->writeReg32(0x60034, 0x00800000);
		NGreen::callback->writeReg32(0x60040, 0x0007C1CD);
		NGreen::callback->writeReg32(0x60044, 0x00080000);
		// Panel power sequencer
		NGreen::callback->writeReg32(0x61204, 0x00000067);
		NGreen::callback->writeReg32(0x61208, 0x07D00001);
		// South display engine / PCH
		NGreen::callback->writeReg32(0x46140, 0x10000000);
		NGreen::callback->writeReg32(0x45400, 0x00000401);
		NGreen::callback->writeReg32(0x45404, 0x00000C03);
		NGreen::callback->writeReg32(0x45408, 0x40000000);
		NGreen::callback->writeReg32(0x4540C, 0x00000401);
		// Enable DMC
		NGreen::callback->writeReg32(0x45520, 2);
		SYSLOG("ngreen", "hwInitCState: ADL-P DMC loaded");

	} else {
		// ── skip (default, safe fallback) ──
		// Proven working: just let original run + configure AUX.
		// No DMC blob, no display engine register writes.
		SYSLOG("ngreen", "hwInitCState: skip (safe fallback), passthrough original");
		FunctionCast(hwInitializeCState, callback->ohwInitializeCState)(that);
	}

	hwConfigureCustomAUX(that, true);
	SYSLOG("ngreen", "hwInitCState: done");
}

/* safe working one .. don't remove
void Gen11::hwInitializeCState(void *that)
{
	// For all paths (ICL, TGL/ADL-P):
	//   1. Let the original run — it loads CSR_PATCH when B48=1, or is a no-op if B48=0.
	//   2. Then configure AUX registers for the actual hardware (CE4-driven map selection).
	// We used to load a custom TGL DMC blob here, but that blob overwrites 0x80000-0x83244
	// with TGL-specific microcode that is wrong on ADL-P and can disrupt the display engine
	// state that BIOS left operational (including the AUX lane).
	FunctionCast(hwInitializeCState, callback->ohwInitializeCState)(that);
	hwConfigureCustomAUX(that, true);
}*/
	/*
	// B48 = ccont[0xB48]: must be 1 for original hwInitializeCState to do anything.
	// CE4 = ccont[0xCE4]: 0 → AUX alt map (0x863xx), non-zero → primary map (0x800xx).
	SYSLOG("ngreen", "hwInitCState B38=%d C9C=%d",
		getMember<int>(ccont, 0xB38), getMember<int>(ccont, 0xC9C));
	SYSLOG("ngreen", "hwInitCState B48=%d CE4=%d",
		getMember<int>(ccont, 0xB48), getMember<int>(ccont, 0xCE4));

	// Boot-arg "ngreen-dmc":
	//   not set or "skip" → force B48=0, skip TGL CSR load (default, safe for RPL)
	//   "tgl"             → let original run with B48 intact (loads TGL DMC)
	//   "adlp"            → load ADL-P v2.16 DMC blob manually, skip original CSR
	char dmcArg[16] = {};
	bool loadOriginal = true;
	int origB48 = getMember<int>(that, 0xB48);

	if (PE_parse_boot_argn("ngreen-dmc", dmcArg, sizeof(dmcArg))) {
		if (dmcArg[0] == 't' || dmcArg[0] == 'T') {
			// Let the original TGL CSR load run (B48 untouched)
			SYSLOG("ngreen", "hwInitCState: ngreen-dmc=tgl, letting original CSR load run (B48=%d)", origB48);
		} else if (dmcArg[0] == 'a' || dmcArg[0] == 'A') {
			// Load ADL-P DMC manually, skip original
			SYSLOG("ngreen", "hwInitCState: ngreen-dmc=adlp, loading ADL-P v2.16 DMC");
			getMember<int>(that, 0xB48) = 0;
			loadOriginal = false;
			unsigned long uVar3 = 0;
			do {
				FastWriteRegister32(ccont, uVar3 + 0x80000, *(const uint32_t *)((const char *)adlp_dmc_ver2_16_bin + uVar3));
				uVar3 = uVar3 + 4;
			} while (uVar3 < adlp_dmc_ver2_16_bin_s);
			NGreen::callback->writeReg32(0x45520, 2);
		} else {
			// "skip" or unknown → skip CSR load
			SYSLOG("ngreen", "hwInitCState: ngreen-dmc=%s, skipping CSR load (B48 forced 0)", dmcArg);
			getMember<int>(that, 0xB48) = 0;
		}
	} else {
		// Default: skip TGL CSR load (wrong firmware for RPL)
		SYSLOG("ngreen", "hwInitCState: no boot-arg, skipping CSR load (B48 %d->0)", origB48);
		getMember<int>(that, 0xB48) = 0;
	}

	FunctionCast(hwInitializeCState, callback->ohwInitializeCState)(that);
	getMember<int>(that, 0xB48) = origB48;  // restore
	hwConfigureCustomAUX(that, true);
}*/


/*void Gen11::hwInitializeCState()
{
	//FunctionCast(hwInitializeCState, callback->ohwInitializeCState)( );
	
	//const auto &fw = getFWByName("adlp_dmc.bin");
	

	unsigned long uVar3 = 0;
	do {
	  FastWriteRegister32(ccont,uVar3+0x80000, *(uint *)((long)tgl_dmc_ver2_12_bin + uVar3));
	  uVar3 = uVar3 + 4;
	} while (uVar3 != (tgl_dmc_ver2_12_bin_s/4));
	
	//some adlp addresses
	/*NGreen::callback->writeReg32(0x8f074,0x86fc0);
	NGreen::callback->writeReg32(0x8f004,0x1240108);
	NGreen::callback->writeReg32(0x8f034,0xc003b400);
	
	
	//tgl address
	NGreen::callback->writeReg32(0x8f074,0x6fc0);
	NGreen::callback->writeReg32(0x8f004,0xa40088);
	NGreen::callback->writeReg32(0x8f034,0xc003b400);
	
	//common
	NGreen::callback->writeReg32(0x45520,2);
	
	hwConfigureCustomAUX(ccont,true);
	
}*/

void Gen11::hwConfigureCustomAUX(void *that,bool param_1)
{
	SYSLOG("ngreen", "hwAUX p1=%d CE4=%d",
		(int)param_1, getMember<int>(that, 0xCE4));

	// Pure passthrough — V12 showed that the native "Custom AUX enable" logic works
	// correctly on ADL-P hardware. The 0x863xx PHY writes added in V12 broke EDID
	// (56283 µs failure). Native-only: EDID succeeded in 3663 µs on same hardware.
	if (callback->ohwConfigureCustomAUX)
		FunctionCast(hwConfigureCustomAUX, callback->ohwConfigureCustomAUX)(that, param_1);
}

void Gen11::FastWriteRegister32(void *that,unsigned long param_1,uint32_t param_2)
{
	if (param_1 == 0x70188) { // PLANE_STRIDE Pipe A Plane 1
		UInt32 linear = param_2 * 8;
		DBGLOG("ngreen", "FastWrite PLANE_STRIDE fixup: 0x%x -> 0x%x (linear)", param_2, linear);
		param_2 = linear;
	}
	if (param_1 == 0x70180) { // PLANE_CTL Pipe A Plane 1
		param_2 &= ~(0x7u << 12); // clear tiling bits → linear
		DBGLOG("ngreen", "FastWrite PLANE_CTL fixup: forced linear tiling");
	}
	return FunctionCast(FastWriteRegister32, callback->oFastWriteRegister32)(that,param_1,param_2 );
}

int Gen11::hasExternalDispla()
{

	return 1;
}

uint8_t Gen11::hwRegsNeedUpdate
		  (void *that,void *param_1,
		   void *param_2,void *param_3,void *param_4,
		   void *param_5)
{
	// Return the original result so that register reprogramming proceeds normally.
	// The lane count mismatch (4→2) that previously broke the display is now fixed
	// by the computeLaneCount hook forcing 4 lanes.  Without register updates, the
	// plane surface address and stride never get written, leaving the stale BIOS
	// framebuffer on screen (grey/black vertical bars with only cursor visible).
	return FunctionCast(hwRegsNeedUpdate, callback->ohwRegsNeedUpdate)(that, param_1, param_2, param_3, param_4, param_5);
}

void Gen11::computeLaneCount(void *that, const void *timing, unsigned int linkRate, unsigned int bpp, unsigned int *laneCount) {
	FunctionCast(computeLaneCount, callback->ocomputeLaneCount)(that, timing, linkRate, bpp, laneCount);
	// BIOS trains the eDP link at 4 lanes (HBR3 x4).  The driver correctly computes
	// that 2 lanes suffice for 285.6 MHz @ 24 bpp, but writing 2 into DDI_FUNC_CTL
	// without link retraining causes a PHY/transcoder lane count mismatch.
	// Force 4 lanes to match the BIOS-trained link configuration.
	if (laneCount && *laneCount < 4) {
		DBGLOG("ngreen", "computeLaneCount: forcing lane count from %u to 4", *laneCount);
		*laneCount = 4;
	}
}

long blti=0;

uint8_t Gen11::isPanelPowerOn()
{
	return 1;
}

// Stub that does nothing (void)
int Gen11::alwaysReturnSuccess(void *) { return 0;}

//****

uint8_t Gen11::SetupDPSSTTimings(void *that,void *param_1,void *param_2,void *param_3){
	return FunctionCast(SetupDPSSTTimings, callback->oSetupDPSSTTimings)(that,param_1,param_2,param_3);
}

uint32_t Gen11::validateDetailedTiming(void *that,void *param_1,unsigned long param_2) {
	return FunctionCast(validateDetailedTiming, callback->ovalidateDetailedTiming)(that,param_1,param_2);
}

uint8_t Gen11::maxSupportedDepths(void *param_1) {
    auto displayTimingInfo = const_cast<IODetailedTimingInformationV2 *>(reinterpret_cast<const IODetailedTimingInformationV2 *>(param_1));
    if (displayTimingInfo!=nullptr) displayTimingInfo->pixelClock = 785400000;
    auto ret= FunctionCast(maxSupportedDepths, callback->omaxSupportedDepths)(param_1);
	return ret;
}

uint8_t Gen11::validateModeDepth(void *that,void *param_1,uint param_2)
{
    auto displayTimingInfo = const_cast<IODetailedTimingInformationV2 *>(reinterpret_cast<const IODetailedTimingInformationV2 *>(param_1));
    if (displayTimingInfo!=nullptr) displayTimingInfo->pixelClock = 785400000;
	auto ret= FunctionCast(validateModeDepth, callback->ovalidateModeDepth)(that, param_1, param_2);
	return ret;
}


void Gen11::SetupTimings(void *that, void *param_1, void *param_2, void *param_3, void *param_4){
	FunctionCast(SetupTimings, callback->oSetupTimings)(that,param_1,param_2, param_3, param_4);
}

uint8_t Gen11::validateDisplayMode(void *that, int param_1,void *param_2, void *param_3){
	auto displayTimingInfo = const_cast<IODetailedTimingInformationV2 *>(reinterpret_cast<const IODetailedTimingInformationV2 *>(param_3));
	if (displayTimingInfo!=nullptr) displayTimingInfo->pixelClock = 785400000;
	auto ret= FunctionCast(validateDisplayMode, callback->ovalidateDisplayMode)(that,param_1,param_2,param_3);
	return ret;
}

void Gen11::setupDisplayTiming (void *that,void *param_1,
                         void *param_2){
    auto displayTimingInfo = const_cast<IODetailedTimingInformationV2 *>(reinterpret_cast<const IODetailedTimingInformationV2 *>(param_2));
    if (displayTimingInfo!=nullptr) displayTimingInfo->pixelClock = 785400000;
	FunctionCast(setupDisplayTiming, callback->osetupDisplayTiming)(that,param_1,param_2);
    //auto ret= FunctionCast(setupDisplayTiming, callback->osetupDisplayTiming)(that,param_1,param_2);
    //return ret;
}

unsigned long Gen11::getPixelInformation (void *that, uint param_1,int param_2,int param_3, void *param_4){
	return FunctionCast(getPixelInformation, callback->ogetPixelInformation)(that,param_1,param_2, param_3, param_4);
}

void * Gen11::getBlit3DContext(void *that,bool param_1)
{
	//return FunctionCast(getBlit3DContext, callback->ogetBlit3DContext)(that,param_1);
	
	void * blti=*getMember<void **>(that, 0x298);
	if (blti!=0) return blti;//FunctionCast(getBlit3DContext, callback->ogetBlit3DContext)(that,param_1);
	
	void * this_00 = IGHardwareBlit3DContextoperatornew(that,param_1);
	
	IGHardwareExtendedContextinitWithOptions(this_00,that,(void*)callback->Blit3DExtendedCtxParams);

	*getMember<void **>(that, 0x298)=this_00;
	return this_00;
	
}

void * Gen11::getBlit2DContext(void *that,bool param_1)
{
	return FunctionCast(getBlit2DContext, callback->ogetBlit2DContext)(that,param_1);
	
}

void *  Gen11::IGHardwareBlit3DContextoperatornew(void *that,unsigned long param_1)
{
	auto ret=FunctionCast(IGHardwareBlit3DContextoperatornew, callback->oIGHardwareBlit3DContextoperatornew)(that,param_1);
	return ret;
}

void Gen11::IGHardwareBlit3DContextinitialize(void *that)
{
	//FunctionCast(IGHardwareBlit3DContextinitialize, callback->oIGHardwareBlit3DContextinitialize)(that);
	
		*getMember<uint64_t *>(that, 0xe8)= 0;
		*getMember<uint64_t *>(that, 0xf0)= 0;
		*getMember<uint64_t *>(that, 0x100)= 0;
		*getMember<uint64_t *>(that, 0xf8)= 0;
		*getMember<uint64_t *>(that, 0x108)= 0;
		*getMember<uint32_t *>(that, 0x110)= 0;
		
		void *pIVar1 = (void *)IGMappedBuffergetMemory(*getMember<void **>(that, 0xd8));
		blit3d_initialize_scratch_space(pIVar1);
		blit3d_init_ctx(that);
	
}

int Gen11::blit3d_supported(void *param_1,void *param_2)
{
	return 0;
}

uint8_t Gen11::IGMappedBuffergetMemory(void *that)
{
	auto ret=FunctionCast(IGMappedBuffergetMemory, callback->oIGMappedBuffergetMemory)(that);
	return ret;
}

uint8_t Gen11::blit3d_init_ctx(void *that)
{
	auto ret=FunctionCast(blit3d_init_ctx, callback->oblit3d_init_ctx)(that);
	return ret;
}

void  Gen11::initBlitUsage(void *that)
{
	/*void *lVar1;
	
	lVar1 = getBlit2DContext(*getMember<void **>(that, 0x68),true);
	if (lVar1!=0){
		if (*getMember<long*>(lVar1, 0xb8)!=0)
		*getMember<uint32_t*>(that, 0x70) = *(uint32_t *)(*getMember<long*>(lVar1, 0xb8) + 0x178);
	}
	
	lVar1 = getBlit3DContext(*getMember<void **>(that, 0x68),true);
	if (lVar1!=0){
		if (*getMember<long*>(lVar1, 0xb8)!=0)
		*getMember<uint32_t*>(that, 0x74) = *(uint32_t *)(*getMember<long*>(lVar1, 0xb8) + 0x178);
	}*/
	
	FunctionCast(initBlitUsage, callback->oinitBlitUsage)(that);
}
void  Gen11::markBlitUsage(void *that)
{
	FunctionCast(markBlitUsage, callback->omarkBlitUsage)(that);
}

uint32_t  Gen11::IGAccelSegmentResourceListprepare(void *that)
{
		
	/*FunctionCast(initBlitUsage, callback->oinitBlitUsage)(that);
	
	FunctionCast(markBlitUsage, callback->omarkBlitUsage)(that);
	*/
	return 0;
	return FunctionCast(IGAccelSegmentResourceListprepare, callback->oIGAccelSegmentResourceListprepare)(that);
}


void Gen11::blit3d_initialize_scratch_space(void *that)
{
	FunctionCast(blit3d_initialize_scratch_space, callback->oblit3d_initialize_scratch_space)(that);
}

uint8_t Gen11::isPanelPowerOn(void *that)
{
	return 1;//FunctionCast(isPanelPowerOn, callback->oisPanelPowerOn)(that);
}

uint8_t  Gen11::IGHardwareExtendedContextinitWithOptions(void *that,void *param_1,void *param_2)
{
	
	return FunctionCast(IGHardwareExtendedContextinitWithOptions, callback->oIGHardwareExtendedContextinitWithOptions)(that,param_1,param_2);
	
	uint8_t ctx=getMember<uint8_t>(that, 0x6c);
	
	//intel_engine_init_workarounds(engine);
		//engine_fake_wa_init
		/*if (engine->class == COMPUTE_CLASS)
		ccs_engine_wa_init(engine, wal);
		 else if (engine->class == RENDER_CLASS)
		rcs_engine_wa_init(engine, wal);
		 else
		xcs_engine_wa_init(engine, wal);*/
	
	//intel_engine_init_whitelist(engine);
	//intel_engine_init_ctx_wa(engine);
	
	if (ctx==0)// RCS
	{
		
		//engine_fake_wa_init
		uint8_t mocs= 3;
		NGreen::callback->wa_masked_field_set(
					RING_CMD_CCTL(RENDER_RING_BASE),
					CMD_CCTL_MOCS_MASK,
					CMD_CCTL_MOCS_OVERRIDE(mocs, mocs));
		
		
		
		
		//	rcs_engine_wa_init(engine, wal);
		//rcs_engine_wa_init
		//Wa_1606700617:tgl,dg1,adl-p
		NGreen::callback->wa_masked_en(
				 GEN9_CS_DEBUG_MODE1,
				 FF_DOP_CLOCK_GATE_DISABLE);
		
		
		// Wa_1606931601:tgl,rkl,dg1,adl-s,adl-p
		NGreen::callback->wa_mcr_masked_en( GEN8_ROW_CHICKEN2, GEN12_DISABLE_EARLY_READ);


		 // Wa_1407928979:tgl A
		NGreen::callback->wa_write_or( GEN7_FF_THREAD_MODE,
				GEN12_FF_TESSELATION_DOP_GATE_DISABLE);

		//general_render_compute_wa_init
		// Wa_1406941453:tgl,rkl,dg1,adl-s,adl-p
		NGreen::callback->wa_mcr_masked_en(
				 GEN10_SAMPLER_MODE,
				 ENABLE_SMALLPL);
		
		
		// Wa_1409804808
		NGreen::callback->wa_mcr_masked_en( GEN8_ROW_CHICKEN2,
				 GEN12_PUSH_CONST_DEREF_HOLD_DIS);

		// Wa_14010229206 /
		NGreen::callback->wa_mcr_masked_en( GEN9_ROW_CHICKEN4, GEN12_DISABLE_TDL_PUSH);
		
		// Wa_1607297627
		NGreen::callback->wa_masked_en(
				RING_PSMI_CTL(RENDER_RING_BASE),
				GEN12_WAIT_FOR_EVENT_POWER_DOWN_DISABLE |
				GEN8_RC_SEMA_IDLE_MSG_DISABLE);
		
		
		//if (GRAPHICS_VER(i915) >= 9)
		NGreen::callback->wa_masked_en(
				 GEN7_FF_SLICE_CS_CHICKEN1,
				 GEN9_FFSC_PERCTX_PREEMPT_CTRL);
		
		
		
		//tgl_whitelist_build
		//WaAllowPMDepthAndInvocationCountAccessFromUMD
		NGreen::callback->whitelist_reg_ext( PS_INVOCATION_COUNT,
				  RING_FORCE_TO_NONPRIV_ACCESS_RD |
				  RING_FORCE_TO_NONPRIV_RANGE_4);

		
		 // Wa_1808121037:tgl

		NGreen::callback->whitelist_reg( GEN7_COMMON_SLICE_CHICKEN1);

		// Wa_1806527549:tgl
		NGreen::callback->whitelist_reg( HIZ_CHICKEN);

		// Required by recommended tuning setting (not a workaround)
		NGreen::callback->whitelist_reg( GEN11_COMMON_SLICE_CHICKEN3);
		
		
		
		//intel_engine_init_ctx_wa
		//gen12_ctx_workarounds_init
		// * Wa_1409142259:tgl,dg1,adl-p
		
		NGreen::callback->wa_masked_en( GEN11_COMMON_SLICE_CHICKEN3,
									  GEN12_DISABLE_CPS_AWARE_COLOR_PIPE);
		
		/* WaDisableGPGPUMidThreadPreemption:gen12 */
		NGreen::callback->wa_masked_field_set( GEN8_CS_CHICKEN1,
											 GEN9_PREEMPT_GPGPU_LEVEL_MASK,
											 GEN9_PREEMPT_GPGPU_THREAD_GROUP_LEVEL);
		
		//* Wa_16011163337 - GS_TIMER
		NGreen::callback->wa_add(
								GEN12_FF_MODE2,
								~0,
								FF_MODE2_TDS_TIMER_128 | FF_MODE2_GS_TIMER_224,
								0, false);
		
						
	}
	
	if (ctx==1)//CCS
	{
		//return 0;
		//engine_fake_wa_init
		uint8_t mocs= 3;
		NGreen::callback->wa_masked_field_set(
					RING_CMD_CCTL(GEN12_COMPUTE0_RING_BASE),
					CMD_CCTL_MOCS_MASK,
					CMD_CCTL_MOCS_OVERRIDE(mocs, mocs));
		
		//panic("x");
	}
	
	if (ctx==2)//BCS
	{

		//gen12_ctx_gt_mocs_init
		/*table->size  = ARRAY_SIZE(tgl_mocs_table);
		table->table = tgl_mocs_table;
		table->n_entries = GEN9_NUM_MOCS_ENTRIES;
		table->uc_index = 3;*/
		uint8_t mocs;
		//if (engine->class == COPY_ENGINE_CLASS) {
			mocs = 3;
			NGreen::callback->wa_write_clr_set(
					 BLIT_CCTL(BLT_RING_BASE),//engine->mmio_base
					 BLIT_CCTL_MASK,
					 BLIT_CCTL_MOCS(mocs, mocs));
		
	}
	
	if (ctx==3)//VCS
	{
	}
	
	if (ctx==4)//VCS2
	{
	}
	
	if (ctx==5)//VECS
	{
	}
	
	
	
	return FunctionCast(IGHardwareExtendedContextinitWithOptions, callback->oIGHardwareExtendedContextinitWithOptions)(that,param_1,param_2);
}

void * Gen11::ExtendedContextWithOptions(void *param_1)
{
	return FunctionCast(ExtendedContextWithOptions, callback->oExtendedContextWithOptions)(param_1);
}

uint8_t Gen11::enableController(void *that)
{
	if (getMember<uint32_t>(that, 0x1dc)==0) getMember<uint8_t>(that, 0x1e0)=1;
	auto ret= FunctionCast(enableController, callback->oenableController)(that);
	return ret;
}


uint8_t Gen11::setDisplayMode(void *that,int param_1,int param_2)
{
	if (getMember<uint32_t>(that, 0x1dc)==0) getMember<uint8_t>(that, 0x1e0)=1;
	return FunctionCast(setDisplayMode, callback->osetDisplayMode)(that,param_1,param_2 );

}

uint8_t Gen11::connectionChanged(void *that)
{
	
	auto ret= FunctionCast(connectionChanged, callback->oconnectionChanged)(that);
	//getMember<uint8_t>(that, 0x1e0)=1;
	return ret;
}

unsigned long  Gen11::allocateDisplayResources(void *that)
{
	auto ret=FunctionCast(allocateDisplayResources, callback->oallocateDisplayResources)(that);
	
	//icl_display_core_init
	NGreen::callback->writeReg32(DC_STATE_EN,0);
	
	/* Wa_14011294188:ehl,jsl,tgl,rkl,adl-s */
	/*if (INTEL_PCH_TYPE(dev_priv) >= PCH_TGP &&
		INTEL_PCH_TYPE(dev_priv) < PCH_DG1)*/
	NGreen::callback->intel_de_rmw( SOUTH_DSPCLK_GATE_D, 0,
				 PCH_DPMGUNIT_CLOCK_GATE_DISABLE);
	
	uint32_t reg,reset_bits;
	reg = HSW_NDE_RSTWRN_OPT;
	reset_bits = RESET_PCH_HANDSHAKE_ENABLE;
	NGreen::callback->intel_de_rmw( reg, reset_bits, reset_bits);
	
	//intel_cdclk_init_hw(dev_priv);
	//gen12_dbuf_slices_config
	NGreen::callback->intel_de_rmw(_DBUF_CTL_S1,
				 DBUF_TRACKER_STATE_SERVICE_MASK,
				 DBUF_TRACKER_STATE_SERVICE(8));
	
	NGreen::callback->intel_de_rmw(_DBUF_CTL_S2,
				 DBUF_TRACKER_STATE_SERVICE_MASK,
				 DBUF_TRACKER_STATE_SERVICE(8));
	
	//gen9_dbuf_enable(dev_priv);
	reg = _DBUF_CTL_S1;
	NGreen::callback->intel_de_rmw(reg, DBUF_POWER_REQUEST,
			  DBUF_POWER_REQUEST);
	NGreen::callback->readReg32(reg);
	//intel_de_posting_read(dev_priv, reg);
	IODelay(10);
	//state = intel_de_read(dev_priv, reg) & DBUF_POWER_STATE;
	reg = _DBUF_CTL_S2;
	NGreen::callback->intel_de_rmw(reg, DBUF_POWER_REQUEST,
			  DBUF_POWER_REQUEST);
	NGreen::callback->readReg32(reg);
	IODelay(10);
	
	
	//icl_mbus_init
	unsigned long abox_mask = GENMASK(2, 1);//DISPLAY_INFO(dev_priv)->abox_mask;
	int config, i;
	
	unsigned long abox_regs=abox_mask;
	uint32_t mask = MBUS_ABOX_BT_CREDIT_POOL1_MASK |
		MBUS_ABOX_BT_CREDIT_POOL2_MASK |
		MBUS_ABOX_B_CREDIT_MASK |
		MBUS_ABOX_BW_CREDIT_MASK;
	uint32_t val = MBUS_ABOX_BT_CREDIT_POOL1(16) |
		MBUS_ABOX_BT_CREDIT_POOL2(16) |
		MBUS_ABOX_B_CREDIT(1) |
		MBUS_ABOX_BW_CREDIT(1);

	//if (DISPLAY_VER(dev_priv) == 12)
	//	abox_regs |= BIT(0);

	for_each_set_bit(i, &abox_regs, sizeof(abox_regs))
	NGreen::callback->intel_de_rmw( MBUS_ABOX_CTL(i), mask, val);
	

	//tgl_bw_buddy_init
	enum intel_dram_type type = INTEL_DRAM_DDR4;
	uint8_t num_channels = 2; //INTEL_DRAM_DDR4
	const struct buddy_page_mask *table=tgl_buddy_page_masks;
	
	
	for (config = 0; table[config].page_mask != 0; config++)
		if (table[config].num_channels == num_channels &&
			table[config].type == type)
			break;
	
		for_each_set_bit(i, &abox_mask, sizeof(abox_mask)) {
		NGreen::callback->writeReg32( BW_BUDDY_PAGE_MASK(i),
					   table[config].page_mask);

			/* Wa_22010178259:tgl,dg1,rkl,adl-s */
		NGreen::callback->intel_de_rmw( BW_BUDDY_CTL(i),
						 BW_BUDDY_TLB_REQ_TIMER_MASK,
						 BW_BUDDY_TLB_REQ_TIMER(0x8));
		}

	
	/* Wa_14011508470:tgl,dg1,rkl,adl-s,adl-p,dg2 */
	//if (IS_DISPLAY_VER_FULL(dev_priv, IP_VER(12, 0), IP_VER(13, 0)))
	/*NGreen::callback->intel_de_rmw( GEN11_CHICKEN_DCPR_2, 0,
				 DCPR_CLEAR_MEMSTAT_DIS | DCPR_SEND_RESP_IMM |
				 DCPR_MASK_LPMODE | DCPR_MASK_MAXLATENCY_MEMUP_CLR);*/
	
	/* * Display WA #1185 WaDisableDARBFClkGating:glk,icl,ehl,tgl
	 * Also known as Wa_14010480278.
	 */
	//if (IS_DISPLAY_VER(i915, 10, 12))
	//NGreen::callback->intel_de_rmw( GEN9_CLKGATE_DIS_0, 0, DARBF_GATING_DIS);
	
	/* Wa_14013723622 */
	NGreen::callback->intel_de_rmw( CLKREQ_POLICY, CLKREQ_POLICY_MEM_UP_OVRD, 0);
	
	
	return ret;
}


	
void Gen11::IGScheduler5resume(void *that) {
		
		void *accelerator = getMember<void *>(that, 0x10);
		struct IGHwCsDesc *descArray = (struct IGHwCsDesc *)callback->kIGHwCsDesc;
		uint32_t mode = _MASKED_BIT_ENABLE(GEN11_GFX_DISABLE_LEGACY_MODE);

		for (int i = 0; i < 6; i++) {
			struct IGHwCsDesc *desc = &descArray[i];

			// --- FILTER: ONLY RCS AND BCS ---
			if (desc->type == kIGHwCsTypeRCS) {
				//FunctionCast(SafeForceWake, callback->oSafeForceWake)(accelerator, true, 0);
			} else if (desc->type == kIGHwCsTypeBCS) {
				//FunctionCast(SafeForceWake, callback->oSafeForceWake)(accelerator, true, 1);
			} else {
				continue;
			}

					uint32_t ringBase = desc->mmioExecListControl - 0x3c;
					
					// USE THE CORRECT STRUCT MEMBER FOR HWS
					// The dump proves mmioGlobalStatusPage holds the offset 0x2080
					uint32_t hwsRegisterOffset = desc->mmioGlobalStatusPage;
					
					// READ SHADOW REGISTER
					// We use the offset from the struct (0x2080) to index the shadow map
					uint32_t hwsAddr = getMember<uint32_t>(accelerator, 0x1240 + hwsRegisterOffset);
					
					// WRITE TO HARDWARE
					NGreen::callback->writeReg32(hwsRegisterOffset, hwsAddr);
					NGreen::callback->readReg32(hwsRegisterOffset);

					// REST OF INIT (Using explicit offsets from struct where possible)
					NGreen::callback->writeReg32(ringBase + 0x98, ~0u); // HWSTAM
					NGreen::callback->writeReg32(desc->mmioGfxMode, mode); // GFX Mode
					NGreen::callback->writeReg32(ringBase + 0x9c, _MASKED_BIT_DISABLE(STOP_RING)); // MI Mode
					
					NGreen::callback->writeReg32(desc->mmioErrorIdentity, ~0u); // EMR
					NGreen::callback->writeReg32(desc->mmioErrorMask, ~0u);    // EIR
					NGreen::callback->writeReg32(desc->mmioErrorIdentity, ~0x1); // I915_ERROR_INSTRUCTION
		}
	
	FunctionCast(IGScheduler5resume, callback->oIGScheduler5resume)(that);
	
}

unsigned long Gen11::resetGraphicsEngine(void *that,void *param_1)
{
	
	//GT workarounds: the list of these WAs is applied whenever these registers
	//*   revert to their default values: on GPU reset, suspend/resume [1]_, etc.
	//gen12_gt_workarounds_init
	/* Wa_14011060649:tgl,rkl,dg1,adl-s,adl-p */
	//wa_14011060649(gt, wal);
	
	NGreen::callback->wa_write_or( VDBOX_CGCTL3F10(RENDER_RING_BASE), IECPUNIT_CLKGATE_DIS);
	NGreen::callback->wa_write_or( VDBOX_CGCTL3F10(BLT_RING_BASE), IECPUNIT_CLKGATE_DIS);
	NGreen::callback->wa_write_or( VDBOX_CGCTL3F10(GEN11_VEBOX_RING_BASE), IECPUNIT_CLKGATE_DIS);

	/* Wa_14011059788:tgl,rkl,adl-s,dg1,adl-p */
	NGreen::callback->wa_mcr_write_or( GEN10_DFR_RATIO_EN_AND_CHICKEN, DFR_DISABLE);

	/*
	 * Wa_14015795083
	 */
	NGreen::callback->wa_add( GEN7_MISCCPCTL, GEN12_DOP_CLOCK_GATE_RENDER_ENABLE,
		   0, 0, false);
	
	// V25: GT slice/engine workarounds moved here from AppleIntelBaseControllerstart.
	// These registers are in the GT power domain (0x0-0x7FFF) and need ForceWake to be held.
	// resetGraphicsEngine is called during ring init when ForceWake IS held.
	
	/* Wa_1409142259:tgl,dg1,adl-p - disable CPS aware color pipe */
	NGreen::callback->wa_masked_en(GEN11_COMMON_SLICE_CHICKEN3,
			GEN12_DISABLE_CPS_AWARE_COLOR_PIPE);

	/* WaDisableGPGPUMidThreadPreemption:gen12 */
	NGreen::callback->wa_masked_field_set(GEN8_CS_CHICKEN1,
				GEN9_PREEMPT_GPGPU_LEVEL_MASK,
				GEN9_PREEMPT_GPGPU_THREAD_GROUP_LEVEL);

	// MCR selector: target slice 0, subslice 0 (known valid on RPL-P with 1 slice)
	uint32_t mcr = GEN8_MCR_SLICE(0) | GEN8_MCR_SUBSLICE(0);
	uint32_t mcr_mask = GEN8_MCR_SLICE_MASK | GEN8_MCR_SUBSLICE_MASK;
	NGreen::callback->wa_write_clr_set(GEN8_MCR_SELECTOR, mcr_mask, mcr);
	
	/* rcs_engine_wa_init - per-context preemption control */
	NGreen::callback->wa_masked_en(GEN7_FF_SLICE_CS_CHICKEN1,
			GEN9_FFSC_PERCTX_PREEMPT_CTRL);
	
	SYSLOG("ngreen", "resetGraphicsEngine: GT workarounds applied (with ForceWake)");
	
	auto ret=FunctionCast(resetGraphicsEngine, callback->oresetGraphicsEngine)(that,param_1);
	return ret;
}

typedef enum AGDCVendorClass {
	kAGDCVendorClassReserved,
	kAGDCVendorClassIntegratedGPU,
	kAGDCVendorClassDiscreteGPU,
	kAGDCVendorClassOtherHW,
	kAGDCVendorClassOtherSW,
	kAGDCVendorClassAppleGPUPolicyManager,
	kAGDCVendorClassAppleGPUPowerManager,
	kAGDCVendorClassGPURoot,
	kAGDCVendorClassAppleGPUWrangler,
	kAGDCVendorClassAppleMuxControl,
} AGDCVendorClass_t;

typedef struct AGDCVendorInfo {
	union {
		struct {
			UInt16 Minor;
			UInt16 Major;
		};
		UInt32 Raw;
	} Version;
	char VendorString[32];
	UInt32 VendorID;
	AGDCVendorClass_t VendorClass;
} AGDCVendorInfo_t;

uint32_t
Gen11::IntelFBClientControldoAttribute
		  (void *that,uint param_1,unsigned long *param_2,unsigned long param_3,unsigned long *param_4,
		   unsigned long *param_5,void *param_6)
{

	/*if (param_1 == 0x923) {
		return kIOReturnUnsupported;
	}*/
	
	auto ret=FunctionCast(IntelFBClientControldoAttribute, callback->oIntelFBClientControldoAttribute)(that,param_1,param_2,param_3,param_4,param_5,param_6);
	
	if (param_1 == 1)//0x2001)
	if (param_5 != (unsigned long *)0x0)
		if (0x2b < *param_5) {
			//memset(param_4,0,0x2c);
			AGDCVendorInfo *v=(AGDCVendorInfo*)param_4;
			v->Version.Raw=0;
			v->Version.Major=0;
			v->Version.Minor=0;
			v->VendorID=0x8086;
			*v->VendorString=*(char*)"INTEL";
			v->VendorClass=kAGDCVendorClassIntegratedGPU;
			//*(mach_vm_address_t*)v=callback->IntelFBClientControl11doAttribut;
		return 0;
	}
	return ret;
}

int hw=1;
int Gen11::hwSetMode
		  (void *that,void *param_1,
		   void *param_2,void *param_3)
{
	auto ret= FunctionCast(hwSetMode, callback->ohwSetMode)(that, param_1, param_2, param_3);
	if (hw)
		enablePipe(that, param_1, param_2, param_3);
	hw=0;
	return ret;
}

void Gen11::enablePipe
		  (void *that,void *param_1,
		   void *param_2,void *param_3)
{
	return FunctionCast(enablePipe, callback->oenablePipe)(that, param_1, param_2, param_3);
}

uint8_t Gen11::beginReset(void *that)
{
	auto ret= FunctionCast(beginReset, callback->obeginReset)(that);
	
	/*static const struct intel_device_info tgl_info = {
		GEN12_FEATURES,
		PLATFORM(INTEL_TIGERLAKE),
		.platform_engine_mask =
			BIT(RCS0) | BIT(BCS0) | BIT(VECS0) | BIT(VCS0) | BIT(VCS2),
	};*/
	
	
	
	/*if (GRAPHICS_VER_FULL(dev_priv) >= IP_VER(12, 10))
		dg1_irq_reset(dev_priv);
	else if (GRAPHICS_VER(dev_priv) >= 11)
		gen11_irq_reset(dev_priv);*/
	
	//dg1_irq_reset(struct drm_i915_private *dev_priv)
	NGreen::callback->writeReg32( DG1_MSTR_TILE_INTR, 0);
	
	//gen11_irq_reset
	
	NGreen::callback->writeReg32( GEN11_GFX_MSTR_IRQ, 0);
	
	//gen11_gt_irq_reset(gt);
	// Disable RCS, BCS, VCS and VECS class engines.
	NGreen::callback->writeReg32( GEN11_RENDER_COPY_INTR_ENABLE, 0);
	NGreen::callback->writeReg32( GEN11_VCS_VECS_INTR_ENABLE,	  0);

	// Restore masks irqs on RCS, BCS, VCS and VECS engines.
	NGreen::callback->writeReg32( GEN11_RCS0_RSVD_INTR_MASK,	~0);
	NGreen::callback->writeReg32( GEN11_BCS_RSVD_INTR_MASK,	~0);
	
	NGreen::callback->writeReg32( GEN11_VCS0_VCS1_INTR_MASK,	~0);
	NGreen::callback->writeReg32( GEN11_VCS2_VCS3_INTR_MASK,	~0);
	
	//if (HAS_ENGINE(gt, VECS2) || HAS_ENGINE(gt, VECS3))
	NGreen::callback->writeReg32( GEN12_VECS2_VECS3_INTR_MASK, ~0);
	
	NGreen::callback->writeReg32( GEN11_GPM_WGBOXPERF_INTR_ENABLE, 0);
	NGreen::callback->writeReg32( GEN11_GPM_WGBOXPERF_INTR_MASK,  ~0);
	NGreen::callback->writeReg32( GEN11_GUC_SG_INTR_ENABLE, 0);
	NGreen::callback->writeReg32( GEN11_GUC_SG_INTR_MASK,  ~0);

	NGreen::callback->writeReg32( GEN11_CRYPTO_RSVD_INTR_ENABLE, 0);
	NGreen::callback->writeReg32( GEN11_CRYPTO_RSVD_INTR_MASK,  ~0);
	
	//gen11_display_irq_reset(dev_priv);
	
	NGreen::callback->writeReg32( GEN11_GFX_MSTR_IRQ, ~0);

	return ret;
}
											
void Gen11::endReset(void *that)
{
	FunctionCast(endReset, callback->oendReset)(that);
	
	//void gen11_gt_irq_postinstall(struct intel_gt *gt)
	uint32_t irqs = GT_RENDER_USER_INTERRUPT;
	uint32_t dmask,smask;
	
	dmask = irqs << 16 | irqs;
	smask = irqs << 16;
	
	
	
	// Enable RCS, BCS, VCS and VECS class interrupts.
	NGreen::callback->writeReg32( GEN11_RENDER_COPY_INTR_ENABLE, dmask);
	NGreen::callback->writeReg32( GEN11_VCS_VECS_INTR_ENABLE, dmask);

		// Unmask irqs on RCS, BCS, VCS and VECS engines.
	NGreen::callback->writeReg32( GEN11_RCS0_RSVD_INTR_MASK, ~smask);
	NGreen::callback->writeReg32( GEN11_BCS_RSVD_INTR_MASK, ~smask);

	NGreen::callback->writeReg32( GEN11_VCS0_VCS1_INTR_MASK, ~dmask);
	NGreen::callback->writeReg32( GEN11_VCS2_VCS3_INTR_MASK, ~dmask);
	
	NGreen::callback->writeReg32( GEN11_VECS0_VECS1_INTR_MASK, ~dmask);
	
		//if (HAS_ENGINE(gt, VECS2) || HAS_ENGINE(gt, VECS3))
	NGreen::callback->writeReg32( GEN12_VECS2_VECS3_INTR_MASK, ~dmask);
	
	
	//gen11_de_irq_postinstall(dev_priv);
	NGreen::callback->writeReg32( GEN11_GFX_MSTR_IRQ, GEN11_MASTER_IRQ);
	
	NGreen::callback->writeReg32( DG1_MSTR_TILE_INTR, DG1_MSTR_IRQ);
}
