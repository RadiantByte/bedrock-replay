#ifndef GLOSSHOOK_H
#define GLOSSHOOK_H

#ifndef __ANDROID__
#error GlossHook only support android
#else
#if !(defined __arm__) && !(defined __aarch64__)
#error GlossHook only support arm and arm64
#endif
#endif

#if (defined __clang__) || (defined __GNUC__)
#define GLOSS_API __attribute__((visibility("default")))
#else
#define GLOSS_API
#endif

#include <stdint.h>
#include <stdbool.h>
#include <unistd.h> 

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __arm__
#define GET_INST_SET(addr) (addr & 1 ? i_set::I_THUMB : i_set::I_ARM)  
#endif 

	typedef enum { I_NONE = 0, I_THUMB, I_ARM, I_ARM64 } i_set;  

	typedef void* GHandle;  

	typedef void* GHook;  

	typedef struct PermissionFlags  
	{
		bool bRead : 1;
		bool bWrite : 1;
		bool bExecute : 1;
		bool bPrivate : 1;
		bool bShared : 1;
		bool align : 3;
	} p_flag;

	 
	typedef struct GlossRegister
	{
		 
		 
#ifdef __arm__
		enum e_reg { R0 = 0, R1, R2, R3, R4, R5, R6, R7, R8, R9, R10, R11, FP = R11, R12, IP = R12, R13, SP = R13, R14, LR = R14, R15, PC = R15, CPSR, MAX_REG };

		union {
			uint32_t r[MAX_REG];
			struct { uint32_t r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12, sp, lr, pc, cpsr; } regs;
		};
#elif __aarch64__
		enum e_reg {
			X0 = 0, X1, X2, X3, X4, X5, X6, X7, X8, X9, X10, X11, X12, X13, X14, X15, X16, X17, X18, X19, X20, X21, X22, X23, X24, X25, X26, X27, X28, X29, FP = X29,
			X30, LR = X30, X31, SP = X31, PC, CPSR, MAX_REG,
			Q0 = X0, Q1 = X1, Q2 = X2, Q3 = X3, Q4 = X4, Q5 = X5, Q6 = X6, Q7 = X7, Q8 = X8, Q9 = X9, Q10 = X10, Q11 = X11, Q12 = X12, Q13 = X13, Q14 = X14, Q15 = X15,
			Q16 = X16, Q17 = X17, Q18 = X18, Q19 = X19, Q20 = X20, Q21 = X21, Q22 = X22, Q23 = X23, Q24 = X24, Q25 = X25, Q26 = X26, Q27 = X27, Q28 = X28, Q29 = X29, Q30 = X30, Q31 = X31,
			MAX_Q_REG
		};

		typedef union {
			__uint128_t q;
			double d[2];
			float f[4];
		} __qreg;

		typedef union {
			uint64_t x;
			uint32_t w[2];
		} __xreg;

		union {
			struct {
				__uint128_t q[MAX_Q_REG];
				uint64_t x[MAX_REG];
			} r;

			struct {
				__qreg q0, q1, q2, q3, q4, q5, q6, q7, q8, q9, q10, q11, q12, q13, q14, q15, q16, q17, q18, q19, q20, q21, q22, q23, q24, q25, q26, q27, q28, q29, q30, q31;
				__xreg x0, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14, x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25, x26, x27, x28, x29;
				__xreg lr, sp; uint64_t pc, cpsr;
			} regs;
		};
#endif 
	} gloss_reg;

	 
	 
	 
	GLOSS_API void GlossSetLog(bool enable);

	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 

	 








	GLOSS_API uintptr_t GlossFindLibMapping(const char* lib_name, pid_t pid, char* lib_path, size_t* lib_mem_len);

	 









	GLOSS_API GHandle GlossOpen(const char* lib_name);

	 










	GLOSS_API int GlossClose(GHandle handle, bool is_dlclose);

	 





	GLOSS_API uintptr_t GlossGetLibBias(const char* lib_name);

	 





	GLOSS_API uintptr_t GlossGetLibBiasEx(GHandle handle);

	 





	GLOSS_API const char* GlossGetLibPath(GHandle handle);

	 






	GLOSS_API bool GlossGetLibPathEx(uintptr_t lib_addr, char* path);

	 





	GLOSS_API size_t GlossGetLibFileSize(GHandle handle);

	 










	GLOSS_API uintptr_t GlossSymbol(GHandle handle, const char* symbol, size_t* sym_size);

	 









	GLOSS_API uintptr_t GlossSymbolEx(uintptr_t lib_addr, const char* symbol, size_t* sym_size);

	 










	GLOSS_API bool GlossGot(GHandle handle, const char* symbol, uintptr_t** addr_list, size_t* addr_list_size);

	 










	GLOSS_API bool GlossAddr(uintptr_t lib_addr, uintptr_t* sym_addr, size_t* sym_size, char* sym_name);

	 





	GLOSS_API uintptr_t GlossGetLibEntry(GHandle handle);

	 









	GLOSS_API uintptr_t GlossGetLibSection(const char* lib_name, const char* sec_name, size_t* sec_size);

	 










	GLOSS_API uintptr_t GlossGetLibSegment(const char* lib_name, uint16_t seg_id, uint32_t* seg_type, size_t* seg_size);


	 
	 
	 







	GLOSS_API bool SetMemoryPermission(uintptr_t addr, size_t len, p_flag* type);
	 



	inline bool Unprotect(uintptr_t addr, size_t len)
	{
		return SetMemoryPermission(addr, len, NULL);
	}

	 








	GLOSS_API bool GetMemoryPermission(uintptr_t addr, p_flag* type, pid_t pid, const char* lib_name);
	 



	inline bool IsAddrExecute(uintptr_t addr)
	{
		p_flag type = { 0,0,0,0,0 };
		GetMemoryPermission(addr, &type, -1, NULL);
		return type.bExecute;
	}

	 







	GLOSS_API void WriteMemory(void* addr, void* data, size_t size, bool vp);

	 








	GLOSS_API void* ReadMemory(void* addr, void* data, size_t size, bool vp);

	 







	GLOSS_API void MemoryFill(void* addr, uint8_t value, size_t size, bool vp);


	 
	 
	 




	GLOSS_API void GlossInit(bool is_init_linker);
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 
	 

	 














	GLOSS_API void GlossHookAddTrampolines(const char* lib_name, uintptr_t start_addr, size_t size);

	 















	GLOSS_API GHook GlossHook(void* sym_addr, void* new_func, void** old_func);

	 














	GLOSS_API GHook GlossHookAddr(void* func_addr, void* new_func, void** old_func, bool is_4_byte_hook, i_set mode);

	 













	GLOSS_API GHook GlossHookBranchBL(void* branch_addr, void* new_func, void** old_func, i_set mode);
#ifdef __arm__
	GLOSS_API GHook GlossHookBranchBLX(void* branch_addr, void* new_func, void** old_func, i_set mode);
#endif 

	 





	typedef void (*GlossHookInternalCallback)(gloss_reg* regs, GHook hook);
	 
















	GLOSS_API GHook GlossHookInternal(void* addr, GlossHookInternalCallback new_func, void* ret_addr, bool is_4_byte_hook, i_set mode);

	 













	GLOSS_API GHook GlossHookRedirect(void* addr, void* new_addr, bool is_4_byte_hook, i_set mode);

	 







	GLOSS_API GHook GlossGotHook(void* got_addr, void* new_func, void** old_func);

	 
	 
	union GlossLinkerFuncProxy
	{
		struct GlossDlopenProxy
		{
			 
			void* (*dlopen)(const char* filename, int flags);
			void** old_dlopen;

			 
			 
			 
			void* (*do_dlopen)(const char* filename, int flags, const void* extinfo);
			void** old_do_dlopen;

			 
			 
			void* (*do_dlopen_n)(const char* filename, int flags, const void* extinfo, void* caller_addr);
			void** old_do_dlopen_n;

			 
			 
			 
		} DlopenProxy;

		struct GlossDlsymProxy
		{
			 
			void* (*dlsym)(void* handle, const char* symbol);
			void** old_dlsym;

			 
			 
			bool (*do_dlsym)(void* handle, const char* sym_name, const char* sym_ver, const void* caller_addr, void** symbol);
			void** old_do_dlsym;

			 
			 
			 
			 
		} DlsymProxy;

		 
		struct GlossFuncProxy
		{
			 
			void* linker_func;
			void** old_linker_func;
		} FuncProxy;
	};
	 











	GLOSS_API GHook GlossLinkerHook(const char* dlfunc, GlossLinkerFuncProxy new_dlfunc);

	 







	struct GlossHookCallback_t
	{
		GHook hook;  
		const char* path;  
		void* addr;  
		size_t size;  
	};
	typedef void (*GlossHookCallback)(const GlossHookCallback_t info);

	 













	GLOSS_API GHook GlossHookByName(const char* lib_name, const char* sym_name, void* new_func, void** old_func, GlossHookCallback call_back_func);

	 













	GLOSS_API GHook GlossPltHook(const char* lib_name, const char* sym_name, void* new_func, void** old_func, GlossHookCallback call_back_func);

	 



















	GLOSS_API GHook GlossHookConstructor(const char* lib_name, uintptr_t offset_addr, void* new_func, void** old_func, bool is_4_byte_hook, i_set mode, GlossHookCallback call_back_func);

	 




	GLOSS_API void GlossHookDisable(GHook hook);
	GLOSS_API void GlossHookEnable(GHook hook);
	GLOSS_API void GlossHookDelete(GHook hook);
	 





	GLOSS_API void GlossDisableAllHook(void* addr, i_set mode);
	GLOSS_API void GlossEnableAllHook(void* addr, i_set mode);
	GLOSS_API void GlossDeleteAllHook(void* addr, i_set mode);

	 





	GLOSS_API int GlossHookGetCount(GHook hook);
	 






	GLOSS_API int GlossHookGetTotalCount(void* addr, i_set inst_set);

	 






	GLOSS_API GHook GlossHookGetLastHook(void* addr, i_set inst_set);

	 





	GLOSS_API GHook GlossHookGetPrevHook(GHook hook);

	 





	GLOSS_API GHook GlossHookGetNextHook(GHook hook);

	 





	GLOSS_API void GlossHookReplaceNewFunc(GHook hook, void* new_func);

	 





	GLOSS_API void* GlossHookGetOldFunc(GHook hook);

	 





	GLOSS_API void* GlossHookGetOriglFunc(GHook hook);

	 





	GLOSS_API uint8_t* GlossHookGetInstBuffer(GHook hook);

#ifdef __cplusplus
}
	 


	template <typename T>
	inline static void WriteMemory(uintptr_t addr, T value, bool vp = true)
	{
		WriteMemory(reinterpret_cast<void*>(addr), reinterpret_cast<void*>(&value), sizeof(T), vp);
	}

	 


	template <typename T>
	inline static T ReadMemory(uintptr_t addr, bool vp = true)
	{
		T value;
		ReadMemory(reinterpret_cast<void*>(addr), reinterpret_cast<void*>(&value), sizeof(T), vp);
		return value;
	}


 
 
#ifndef GLOSS_HOOK_INST_H
#define GLOSS_HOOK_INST_H
	
	namespace Gloss {
		namespace Inst {
			int CheckAbsoluteJump(uintptr_t addr);
			int CheckRelativeJump(uintptr_t addr);

			 
			enum class Conds { EQ, NE, CS, HS = CS, CC, LO = CC, MI, PL, VS, VC, HI, LS, GE, LT, GT, LE, AL, NV, MAX_COND };

			 
			enum class Branchs { B_COND16, B_COND, B_16, B, BL, BLX, MAX_BRANCH };
			 






			GLOSS_API Branchs GetBranch(uintptr_t addr, i_set mode);

#ifdef __arm__

			 





			GLOSS_API bool IsThumb32(uint32_t addr);

			 





			GLOSS_API void MakeThumb16NOP(uint32_t addr, size_t size);
			GLOSS_API void MakeThumb32NOP(uint32_t addr, size_t size);

			 





			GLOSS_API void MakeThumbRET(uint32_t addr, uint8_t type);

			 







			GLOSS_API uint16_t MakeThumb16B(uint32_t addr, uint32_t dest);
			GLOSS_API uint16_t MakeThumb16BCond(uint32_t addr, uint32_t dest, Conds cond);
			GLOSS_API uint32_t MakeThumb32B(uint32_t addr, uint32_t dest);
			GLOSS_API uint32_t MakeThumb32BCond(uint32_t addr, uint32_t dest, Conds cond);
			GLOSS_API uint32_t MakeThumbBL(uint32_t addr, uint32_t func);
			GLOSS_API uint32_t MakeThumbBL_W(uint32_t addr, uint32_t func);
			GLOSS_API uint32_t MakeThumbBLX(uint32_t addr, uint32_t func);
			GLOSS_API uint32_t MakeThumbBLX_W(uint32_t addr, uint32_t func);

			 








			GLOSS_API uint16_t MakeThumbCB(uint32_t addr, uint32_t dest, gloss_reg::e_reg reg, bool is_cbnz);

			 










			GLOSS_API int8_t MakeThumbAbsoluteJump(uint32_t addr, uint32_t dest);

			 





			GLOSS_API uint32_t GetThumb16BranchDestination(uint32_t addr);
			GLOSS_API uint32_t GetThumb32BranchDestination(uint32_t addr);

			 





			GLOSS_API void MakeArmNOP(uint32_t addr, size_t size);

			 





			GLOSS_API void MakeArmRET(uint32_t addr, uint8_t type);

			 







			GLOSS_API uint32_t MakeArmB(uint32_t addr, uint32_t dest, Conds cond = Conds::AL);
			GLOSS_API uint32_t MakeArmBL(uint32_t addr, uint32_t func, Conds cond = Conds::AL);
			GLOSS_API uint32_t MakeArmBLX(uint32_t addr, uint32_t func);

			 










			GLOSS_API int8_t MakeArmAbsoluteJump(uint32_t addr, uint32_t dest);

			 





			GLOSS_API uint32_t GetArmBranchDestination(uint32_t addr);

#elif __aarch64__

			 





			GLOSS_API void MakeArm64NOP(uint64_t addr, size_t size);

			 





			GLOSS_API void MakeArm64RET(uint64_t addr, uint8_t type);

			 







			GLOSS_API uint32_t MakeArm64B(uint64_t addr, uint64_t dest);
			GLOSS_API uint32_t MakeArm64BCond(uint64_t addr, uint64_t dest, Conds cond);
			GLOSS_API uint32_t MakeArm64BL(uint64_t addr, uint64_t func);

			 









			GLOSS_API uint32_t MakeArm64CB(uint64_t addr, uint64_t dest, gloss_reg::e_reg reg, bool is_cbnz, bool is64);

			 




























			GLOSS_API int8_t MakeArm64AbsoluteJump(uint64_t addr, uint64_t dest, gloss_reg::e_reg reg = gloss_reg::e_reg::X18);  
			GLOSS_API int8_t MakeArm64AbsoluteJumpRet(uint64_t addr, uint64_t dest, gloss_reg::e_reg reg = gloss_reg::e_reg::X18);  
			GLOSS_API int8_t MakeArm64AbsoluteJump32(uint64_t addr, uint64_t dest, gloss_reg::e_reg reg = gloss_reg::e_reg::X18);  
			GLOSS_API int8_t MakeArm64AbsoluteJump128(uint64_t addr, uint64_t dest);  

			 





			GLOSS_API uint64_t GetArm64BranchDestination(uint64_t addr);

#endif 
		}
	}
#endif

#endif
#endif