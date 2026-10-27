/* Original PE64 translation loader: validated archive overlays, no engine patch. */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <bcrypt.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <wchar.h>
#include <stdint.h>
#include <stddef.h>
#include "translation_manifest.h"
#define LOADER_BUILD_ID "3-20261026-independent-exit-code"
#ifdef BUILD_DLL
static HANDLE dll_diagnostic=INVALID_HANDLE_VALUE;
static void dll_record(const char*message){DWORD written;if(dll_diagnostic!=INVALID_HANDLE_VALUE)WriteFile(dll_diagnostic,message,(DWORD)strlen(message),&written,NULL);}
static VOID (WINAPI *real_exit)(UINT);
static BOOL (WINAPI *real_terminate)(HANDLE,UINT);
static BOOL (WINAPI *real_create_process)(LPCWSTR,LPWSTR,LPSECURITY_ATTRIBUTES,LPSECURITY_ATTRIBUTES,BOOL,DWORD,LPVOID,LPCWSTR,LPSTARTUPINFOW,LPPROCESS_INFORMATION);
static void record_exit(const char*kind,DWORD code,void*caller){
 char text[256];snprintf(text,sizeof text,"%s code=%lu (0x%08lx) caller=%p thread=%lu\r\n",kind,(unsigned long)code,(unsigned long)code,caller,(unsigned long)GetCurrentThreadId());dll_record(text);
}
static VOID WINAPI observe_exit(UINT code){record_exit("ExitProcess",code,__builtin_return_address(0));real_exit(code);for(;;)Sleep(INFINITE);}
static BOOL WINAPI observe_terminate(HANDLE process,UINT code){if(GetProcessId(process)==GetCurrentProcessId())record_exit("TerminateProcess(self)",code,__builtin_return_address(0));return real_terminate(process,code);}
static BOOL WINAPI observe_create_process(LPCWSTR application,LPWSTR command,LPSECURITY_ATTRIBUTES process_security,LPSECURITY_ATTRIBUTES thread_security,BOOL inherit,DWORD flags,LPVOID environment,LPCWSTR directory,LPSTARTUPINFOW startup,LPPROCESS_INFORMATION information){
 BOOL result=real_create_process(application,command,process_security,thread_security,inherit,flags,environment,directory,startup,information);DWORD saved=GetLastError();
 char text[256];snprintf(text,sizeof text,"CreateProcessW success=%u child_pid=%lu flags=0x%08lx Win32=%lu\r\n",result,(unsigned long)(result?information->dwProcessId:0),(unsigned long)flags,(unsigned long)saved);dll_record(text);SetLastError(saved);return result;
}
static LONG exception_count;
static LONG CALLBACK observe_exception(PEXCEPTION_POINTERS exception){
 DWORD code=exception->ExceptionRecord->ExceptionCode;
 if((code==0xc0000005||code==0xc00000fd||code==0xc000001d||code==0xc0000374||code==0xc0000409)&&InterlockedIncrement(&exception_count)<=16){
  char text[512];EXCEPTION_RECORD*record=exception->ExceptionRecord;NT_TIB*tib=(NT_TIB*)NtCurrentTeb();
  snprintf(text,sizeof text,"First-chance exception candidate code=0x%08lx address=%p RIP=0x%llx RSP=0x%llx stack_base=%p stack_limit=%p thread=%lu info0=0x%llx info1=0x%llx\r\n",(unsigned long)code,record->ExceptionAddress,(unsigned long long)exception->ContextRecord->Rip,(unsigned long long)exception->ContextRecord->Rsp,tib->StackBase,tib->StackLimit,(unsigned long)GetCurrentThreadId(),(unsigned long long)(record->NumberParameters?record->ExceptionInformation[0]:0),(unsigned long long)(record->NumberParameters>1?record->ExceptionInformation[1]:0));dll_record(text);
 }
 return EXCEPTION_CONTINUE_SEARCH;
}
static HANDLE (WINAPI *real_open)(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
static wchar_t root[32768],stage[32768],font_file[32768],font_family[LF_FACESIZE];
static HFONT (WINAPI *real_font_w)(int,int,int,int,int,DWORD,DWORD,DWORD,DWORD,DWORD,DWORD,DWORD,DWORD,LPCWSTR);
static HANDLE trace_output=INVALID_HANDLE_VALUE;
static BOOL (WINAPI *real_read)(HANDLE,LPVOID,DWORD,LPDWORD,LPOVERLAPPED);
static HANDLE overlay_handles[64];static LONG overlay_count;
static void trace(const char*message){if(trace_output!=INVALID_HANDLE_VALUE){DWORD n;WriteFile(trace_output,message,(DWORD)strlen(message),&n,NULL);}}
static BOOL WINAPI translated_read(HANDLE file,LPVOID data,DWORD bytes,LPDWORD count,LPOVERLAPPED overlap){
 BOOL result=real_read(file,data,bytes,count,overlap);DWORD saved=GetLastError();
 if(result&&count&&trace_output!=INVALID_HANDLE_VALUE){
  LONG total=overlay_count;if(total>64)total=64;
  for(LONG i=0;i<total;i++)if(overlay_handles[i]==file){
   const char*raw=data;BOOL compiled=FALSE;
   for(DWORD n=0;n+27<=*count;n++)if(!memcmp(raw+n,"BurikoCompiledScriptVer1.00",27)){compiled=TRUE;break;}
   if(compiled)trace("Overlay ReadFile consumed raw CompiledScript header.\r\n");
   break;
  }
 }
 SetLastError(saved);return result;
}
static INIT_ONCE font_once=INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK install_font(PINIT_ONCE once,PVOID argument,PVOID*context){
 (void)once;(void)argument;(void)context;
 BOOL result=AddFontResourceExW(font_file,FR_PRIVATE,NULL)>0;
 trace(result?"Private user font loaded.\r\n":"WARNING: private user font could not be loaded.\r\n");
 return result;
}
static HFONT WINAPI translated_font_w(int h,int w,int e,int o,int weight,DWORD italic,DWORD underline,DWORD strike,DWORD charset,DWORD output,DWORD clip,DWORD quality,DWORD pitch,LPCWSTR family){
 if(font_file[0]&&InitOnceExecuteOnce(&font_once,install_font,NULL,NULL)){family=font_family;charset=DEFAULT_CHARSET;}
 return real_font_w(h,w,e,o,weight,italic,underline,strike,charset,output,clip,quality,pitch,family);
}
static HFONT WINAPI translated_font_a(int h,int w,int e,int o,int weight,DWORD italic,DWORD underline,DWORD strike,DWORD charset,DWORD output,DWORD clip,DWORD quality,DWORD pitch,LPCSTR family){
 wchar_t converted[LF_FACESIZE];
 if(!family)converted[0]=0;else if(!MultiByteToWideChar(CP_ACP,0,family,-1,converted,LF_FACESIZE))return NULL;
 return translated_font_w(h,w,e,o,weight,italic,underline,strike,charset,output,clip,quality,pitch,converted);
}
static HANDLE WINAPI redirect_open(LPCWSTR path,DWORD access,DWORD share,LPSECURITY_ATTRIBUTES security,DWORD creation,DWORD attributes,HANDLE template_file){
 if(path&&creation==OPEN_EXISTING&&!(access&(GENERIC_WRITE|FILE_WRITE_DATA|FILE_APPEND_DATA|DELETE))){
  const wchar_t*name=path;
  for(const wchar_t*p=path;*p;p++)if(*p==L'\\'||*p==L'/')name=p+1;
  for(unsigned i=0;i<ARCHIVE_COUNT;i++)if(!_wcsicmp(name,archives[i].name)){
   DWORD full_count=GetFullPathNameW(path,0,NULL,NULL);
   SIZE_T expected_count=wcslen(root)+wcslen(archives[i].name)+11,replacement_count=wcslen(stage)+wcslen(archives[i].name)+2;
   if(!full_count||full_count>32768)break;
   wchar_t*scratch=HeapAlloc(GetProcessHeap(),0,(full_count+expected_count+replacement_count)*sizeof(wchar_t));
   if(!scratch){SetLastError(ERROR_NOT_ENOUGH_MEMORY);return INVALID_HANDLE_VALUE;}
   wchar_t*expected=scratch+full_count,*replacement=expected+expected_count;
   DWORD length=GetFullPathNameW(path,full_count,scratch,NULL);
   if(length&&length<full_count&&swprintf(expected,expected_count,L"%ls\\Archive\\%ls",root,archives[i].name)>=0&&!_wcsicmp(scratch,expected)){
    swprintf(replacement,replacement_count,L"%ls\\%ls",stage,archives[i].name);
    HANDLE result=real_open(replacement,access,share,security,creation,attributes,template_file);DWORD saved=GetLastError();
    static LONG opens;LONG slot=InterlockedIncrement(&opens)-1;
    if(slot<16){char text[192];snprintf(text,sizeof text,"Archive redirect index=%u success=%u Win32=%lu access=0x%08lx attributes=0x%08lx\r\n",i,result!=INVALID_HANDLE_VALUE,(unsigned long)saved,(unsigned long)access,(unsigned long)attributes);dll_record(text);}
    if(result!=INVALID_HANDLE_VALUE&&trace_output!=INVALID_HANDLE_VALUE){LONG index=InterlockedIncrement(&overlay_count)-1;if(index<64)overlay_handles[index]=result;trace("Original engine opened redirected archive.\r\n");}
    HeapFree(GetProcessHeap(),0,scratch);SetLastError(saved);return result;
   }
   HeapFree(GetProcessHeap(),0,scratch);break;
  }
 }
 return real_open(path,access,share,security,creation,attributes,template_file);
}
static BOOL dll_fail(const char*stage){
 DWORD saved=GetLastError();char message[256];DWORD written;
 int length=snprintf(message,sizeof message,"DLL failed: stage=%s Win32=%lu (0x%08lx)\r\n",stage,(unsigned long)saved,(unsigned long)saved);
 if(dll_diagnostic!=INVALID_HANDLE_VALUE){if(length>0)WriteFile(dll_diagnostic,message,(DWORD)length,&written,NULL);CloseHandle(dll_diagnostic);dll_diagnostic=INVALID_HANDLE_VALUE;}
 SetLastError(saved);return FALSE;
}
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,LPVOID reserved){
 (void)reserved;if(reason!=DLL_PROCESS_ATTACH)return TRUE;DisableThreadLibraryCalls(module);
 static wchar_t diagnostic_name[32768];DWORD size=GetEnvironmentVariableW(L"BGI_TRANSLATION_DLL_DIAGNOSTIC",diagnostic_name,32768);
 if(size&&size<32768)dll_diagnostic=CreateFileW(diagnostic_name,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
 dll_record("DLL build=" LOADER_BUILD_ID "\r\n");
 AddVectoredExceptionHandler(1,observe_exception);
 DWORD n=GetEnvironmentVariableW(L"BGI_TRANSLATION_ROOT",root,32768);if(!n||n>=32768)return dll_fail("environment-path");
 n=GetEnvironmentVariableW(L"BGI_TRANSLATION_STAGE",stage,32768);if(!n||n>=32768)return dll_fail("environment-path");
 n=GetEnvironmentVariableW(L"BGI_TRANSLATION_FONT",font_file,32768);if(n>=32768)return dll_fail("font-path");
 if(n){n=GetEnvironmentVariableW(L"BGI_TRANSLATION_FAMILY",font_family,LF_FACESIZE);if(!n||n>=LF_FACESIZE)return dll_fail("font-family");}
 real_font_w=(void*)GetProcAddress(GetModuleHandleW(L"gdi32.dll"),"CreateFontW");if(font_file[0]&&!real_font_w)return dll_fail("resolve-CreateFontW");
 real_open=(void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"CreateFileW");if(!real_open)return dll_fail("resolve-CreateFileW");
 static wchar_t trace_path[32768];n=GetEnvironmentVariableW(L"BGI_TRANSLATION_TRACE",trace_path,32768);
 if(n&&n<32768)trace_output=real_open(trace_path,GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
 real_read=(void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"ReadFile");
 real_exit=(void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"ExitProcess");
 real_terminate=(void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"TerminateProcess");
 real_create_process=(void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"CreateProcessW");
 unsigned char*base=(void*)GetModuleHandleW(NULL);IMAGE_DOS_HEADER*dos=(void*)base;
 if(dos->e_magic!=IMAGE_DOS_SIGNATURE)return dll_fail("main-DOS-header");
 IMAGE_NT_HEADERS64*nt=(void*)(base+dos->e_lfanew);
 if(nt->Signature!=IMAGE_NT_SIGNATURE||nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC)return dll_fail("main-PE64-header");
 {char text[256];snprintf(text,sizeof text,"Main image=%p helper=%p stack_reserve=%llu stack_commit=%llu\r\n",base,module,(unsigned long long)nt->OptionalHeader.SizeOfStackReserve,(unsigned long long)nt->OptionalHeader.SizeOfStackCommit);dll_record(text);}
 DWORD rva=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;if(!rva)return dll_fail("main-import-table");
 IMAGE_IMPORT_DESCRIPTOR*imports=(void*)(base+rva);BOOL found=FALSE;
 for(;imports->Name;imports++){
  if(!imports->OriginalFirstThunk)continue;
  IMAGE_THUNK_DATA64*names=(void*)(base+imports->OriginalFirstThunk),*iat=(void*)(base+imports->FirstThunk);
  for(;names->u1.AddressOfData;names++,iat++){
   if(IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal))continue;
   IMAGE_IMPORT_BY_NAME*symbol=(void*)(base+names->u1.AddressOfData);void*replacement=NULL;
   if(!strcmp((char*)symbol->Name,"CreateFileW")){replacement=redirect_open;found=TRUE;}
   else if(real_exit&&!strcmp((char*)symbol->Name,"ExitProcess"))replacement=observe_exit;
   else if(real_terminate&&!strcmp((char*)symbol->Name,"TerminateProcess"))replacement=observe_terminate;
   else if(real_create_process&&!strcmp((char*)symbol->Name,"CreateProcessW"))replacement=observe_create_process;
   else if(trace_output!=INVALID_HANDLE_VALUE&&real_read&&!strcmp((char*)symbol->Name,"ReadFile"))replacement=translated_read;
   else if(font_file[0]&&!strcmp((char*)symbol->Name,"CreateFontW"))replacement=translated_font_w;
   else if(font_file[0]&&!strcmp((char*)symbol->Name,"CreateFontA"))replacement=translated_font_a;
   else continue;
   DWORD old;if(!VirtualProtect(&iat->u1.Function,sizeof iat->u1.Function,PAGE_READWRITE,&old))return dll_fail("unprotect-IAT");
   iat->u1.Function=(ULONGLONG)(uintptr_t)replacement;
   if(!VirtualProtect(&iat->u1.Function,sizeof iat->u1.Function,old,&old))return dll_fail("restore-IAT-protection");

  }
 }
 if(!found)return dll_fail("CreateFileW-import-absent");
 wchar_t event_name[128];n=GetEnvironmentVariableW(L"BGI_TRANSLATION_READY",event_name,128);
 if(!n||n>=128)return dll_fail("ready-event-name");
 HANDLE event=OpenEventW(EVENT_MODIFY_STATE,FALSE,event_name);
 if(!event)return dll_fail("open-ready-event");
 if(!SetEvent(event)){CloseHandle(event);return dll_fail("signal-ready-event");}CloseHandle(event);
 if(dll_diagnostic!=INVALID_HANDLE_VALUE){DWORD written;const char*message="DLL archive redirect initialization succeeded.\r\n";WriteFile(dll_diagnostic,message,(DWORD)strlen(message),&written,NULL);}
 return TRUE;
}
#else
static HANDLE diagnostic=INVALID_HANDLE_VALUE;
static wchar_t diagnostic_path[32768];
static const char*step="initialize";
static DWORD failure_error=ERROR_SUCCESS;
static NTSTATUS native_status=0;
static void report(const char*message){
 DWORD written;fprintf(stderr,"%s\n",message);
 if(diagnostic!=INVALID_HANDLE_VALUE){WriteFile(diagnostic,message,(DWORD)strlen(message),&written,NULL);WriteFile(diagnostic,"\r\n",2,&written,NULL);}
}
static void start_diagnostic(void){
 DWORD length=GetModuleFileNameW(NULL,diagnostic_path,32768);
 if(length&&length<32600){wchar_t*slash=wcsrchr(diagnostic_path,L'\\');if(slash)wcscpy(slash+1,L"BGI_Chinese-loader.log");}
 diagnostic=CreateFileW(diagnostic_path,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
 if(diagnostic==INVALID_HANDLE_VALUE){length=GetTempPathW(32768,diagnostic_path);if(length&&length<32600){wcscat(diagnostic_path,L"BGI_Chinese-loader.log");diagnostic=CreateFileW(diagnostic_path,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);}}
 report("BGI Chinese loader build=" LOADER_BUILD_ID "; PE64 / MSVCRT / remote ntdll bootstrap.");
}
#define STEP(label) do {step=label;SetLastError(ERROR_SUCCESS);} while(0)
#define REQUIRE(expression) do {if(!(expression)){failure_error=GetLastError();goto done;}}while(0)
#define FAIL(error) do {failure_error=error;goto done;}while(0)
static int hash_file(const wchar_t*path,const unsigned char expected[32]){
 HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);if(file==INVALID_HANDLE_VALUE)return 0;
 BCRYPT_ALG_HANDLE alg=NULL;BCRYPT_HASH_HANDLE hash=NULL;
 unsigned char buffer[65536],digest[32],*object=NULL;DWORD read=0,length=0,returned=0,saved=ERROR_SUCCESS;int ok=0;
 native_status=BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,NULL,0);if(native_status<0)goto done;
 native_status=BCryptGetProperty(alg,BCRYPT_OBJECT_LENGTH,(PUCHAR)&length,sizeof length,&returned,0);if(native_status<0)goto done;
 object=HeapAlloc(GetProcessHeap(),0,length);if(!object){saved=ERROR_NOT_ENOUGH_MEMORY;goto done;}
 native_status=BCryptCreateHash(alg,&hash,object,length,NULL,0,0);if(native_status<0)goto done;
 for(;;){if(!ReadFile(file,buffer,sizeof buffer,&read,NULL)){saved=GetLastError();goto done;}if(!read)break;
  native_status=BCryptHashData(hash,buffer,read,0);if(native_status<0)goto done;
 }
 native_status=BCryptFinishHash(hash,digest,32,0);if(native_status<0)goto done;
 ok=!memcmp(digest,expected,32);if(!ok)saved=ERROR_CRC;
 done:
 if(hash)BCryptDestroyHash(hash);
 if(object)HeapFree(GetProcessHeap(),0,object);
 if(alg)BCryptCloseAlgorithmProvider(alg,0);
 CloseHandle(file);
 if(native_status<0&&!saved)saved=ERROR_GEN_FAILURE;
 SetLastError(saved);return ok;
}
static int extract(unsigned id,const wchar_t*path){
 HRSRC resource=FindResourceW(NULL,MAKEINTRESOURCEW(id),RT_RCDATA);if(!resource)return 0;
 DWORD size=SizeofResource(NULL,resource),written=0;const void*data=LockResource(LoadResource(NULL,resource));if(!data||!size){SetLastError(ERROR_RESOURCE_DATA_NOT_FOUND);return 0;}
 HANDLE file=CreateFileW(path,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);if(file==INVALID_HANDLE_VALUE)return 0;
 int ok=WriteFile(file,data,size,&written,NULL)&&written==size;DWORD saved=GetLastError();if(!ok&&!saved)saved=ERROR_WRITE_FAULT;CloseHandle(file);SetLastError(saved);return ok;
}
/* ASLR-safe: find the actual export-owning local module, then the same module
 * in the target. In a suspended native process ntdll is present even before
 * kernel32's loader initialization. Never reuse this process's function VA. */
static void*find_remote_image(HANDLE process,const wchar_t*name){
 MEMORY_BASIC_INFORMATION info;uintptr_t address=0;
 while(VirtualQueryEx(process,(void*)address,&info,sizeof info)==sizeof info){
  uintptr_t next=(uintptr_t)info.BaseAddress+info.RegionSize;
  if(info.Type==MEM_IMAGE&&info.State==MEM_COMMIT&&info.AllocationBase==info.BaseAddress){
   IMAGE_DOS_HEADER dos;IMAGE_NT_HEADERS64 nt;IMAGE_EXPORT_DIRECTORY exports;SIZE_T read;
   unsigned char*base=info.AllocationBase;char image[128]={0};
   if(ReadProcessMemory(process,base,&dos,sizeof dos,&read)&&read==sizeof dos&&dos.e_magic==IMAGE_DOS_SIGNATURE&&dos.e_lfanew>0&&dos.e_lfanew<0x100000&&
      ReadProcessMemory(process,base+dos.e_lfanew,&nt,sizeof nt,&read)&&read==sizeof nt&&nt.Signature==IMAGE_NT_SIGNATURE&&nt.OptionalHeader.Magic==IMAGE_NT_OPTIONAL_HDR64_MAGIC){
    DWORD rva=nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
    if(rva&&rva<nt.OptionalHeader.SizeOfImage&&ReadProcessMemory(process,base+rva,&exports,sizeof exports,&read)&&read==sizeof exports&&exports.Name<nt.OptionalHeader.SizeOfImage&&
       ReadProcessMemory(process,base+exports.Name,image,sizeof image-1,&read)){
     wchar_t wide[128];if(MultiByteToWideChar(CP_ACP,0,image,-1,wide,128)&&!_wcsicmp(wide,name))return base;
    }
   }
  }
  if(next<=address)break;
  address=next;
 }
 SetLastError(ERROR_MOD_NOT_FOUND);return NULL;
}
static void*remote_export(HANDLE process,DWORD pid,const wchar_t*library,const char*symbol){
 HMODULE local=GetModuleHandleW(library),owner=NULL;if(!local)return NULL;
 FARPROC function=GetProcAddress(local,symbol);if(!function)return NULL;
 if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCWSTR)(uintptr_t)function,&owner))return NULL;
 wchar_t path[32768];DWORD length=GetModuleFileNameW(owner,path,32768);if(!length||length>=32768)return NULL;
 const wchar_t*name=wcsrchr(path,L'\\');name=name?name+1:path;
 HANDLE snapshot=INVALID_HANDLE_VALUE;
 for(unsigned retry=0;retry<8;retry++){snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid);if(snapshot!=INVALID_HANDLE_VALUE||GetLastError()!=ERROR_BAD_LENGTH)break;}
 MODULEENTRY32W entry={.dwSize=sizeof entry};void*result=NULL;
 if(snapshot!=INVALID_HANDLE_VALUE&&Module32FirstW(snapshot,&entry))do{if(!_wcsicmp(entry.szModule,name)){result=entry.modBaseAddr+((uintptr_t)function-(uintptr_t)owner);break;}}while(Module32NextW(snapshot,&entry));
 if(snapshot!=INVALID_HANDLE_VALUE)CloseHandle(snapshot);
 if(!result){void*base=find_remote_image(process,name);if(base)result=(char*)base+((uintptr_t)function-(uintptr_t)owner);}
 SetLastError(result?ERROR_SUCCESS:ERROR_MOD_NOT_FOUND);return result;
}
struct Bootstrap {
 uint64_t function;
 USHORT length,maximum_length;
 DWORD padding;
 uint64_t buffer,module;
 LONG status;
 DWORD spare;
};
_Static_assert(offsetof(struct Bootstrap,length)==8,"UNICODE_STRING layout");
_Static_assert(offsetof(struct Bootstrap,buffer)==16,"Unicode buffer layout");
_Static_assert(offsetof(struct Bootstrap,module)==24,"module output layout");
_Static_assert(offsetof(struct Bootstrap,status)==32,"NTSTATUS output layout");
/* Windows x64 ABI: RCX=context; LdrLoadDll(NULL,NULL,&UNICODE_STRING,&module).
 * This small platform bootstrap is independent of game VM instructions. */
static const unsigned char bootstrap_code[]={
 0x53,0x48,0x83,0xec,0x20,0x48,0x89,0xcb,0x31,0xc9,0x31,0xd2,
 0x4c,0x8d,0x43,0x08,0x4c,0x8d,0x4b,0x18,0xff,0x13,0x89,0x43,0x20,
 0x31,0xc0,0x48,0x83,0xc4,0x20,0x5b,0xc3
};
int wmain(int argc,wchar_t**argv){
 static wchar_t exe[32768],root[32768],stage[32768],temp[32768],path[32768],dll[32768],command[32768],event_name[128];
 PROCESS_INFORMATION process={0};HANDLE injected=NULL,event=NULL;void*remote=NULL;int status=1;
 unsigned extracted=0;BOOL created=FALSE;DWORD deadline=0,thread_exit=STILL_ACTIVE,child_exit=STILL_ACTIVE,process_wait=WAIT_FAILED;
 const wchar_t*target=NULL,*font=NULL,*family=NULL,*trace_path=NULL;
 struct Bootstrap context={0};SIZE_T written=0;start_diagnostic();
 STEP("parse-arguments");
 for(int i=1;i<argc;i++){
  if(!wcscmp(argv[i],L"--version")){report(LOADER_BUILD_ID);status=0;goto done;}
  else if(!wcscmp(argv[i],L"--probe-seconds")&&i+1<argc){wchar_t*end=NULL;deadline=wcstoul(argv[++i],&end,10);if(!deadline||deadline>600||!end||*end)FAIL(ERROR_INVALID_PARAMETER);}
  else if(!wcscmp(argv[i],L"--font")&&i+1<argc)font=argv[++i];
  else if(!wcscmp(argv[i],L"--font-family")&&i+1<argc)family=argv[++i];
  else if(!wcscmp(argv[i],L"--trace")&&i+1<argc)trace_path=argv[++i];
  else if(argv[i][0]!=L'-'&&!target)target=argv[i];else FAIL(ERROR_INVALID_PARAMETER);
 }
 if((font==NULL)!=(family==NULL)||(family&&(!family[0]||wcslen(family)>=LF_FACESIZE)))FAIL(ERROR_INVALID_PARAMETER);
 STEP("configure-user-font");
 if(font){DWORD count=GetFullPathNameW(font,32768,path,NULL);if(!count||count>=32768)FAIL(ERROR_FILENAME_EXCED_RANGE);
  REQUIRE(GetFileAttributesW(path)!=INVALID_FILE_ATTRIBUTES);
  REQUIRE(SetEnvironmentVariableW(L"BGI_TRANSLATION_FONT",path));REQUIRE(SetEnvironmentVariableW(L"BGI_TRANSLATION_FAMILY",family));
 }else{SetEnvironmentVariableW(L"BGI_TRANSLATION_FONT",NULL);SetEnvironmentVariableW(L"BGI_TRANSLATION_FAMILY",NULL);}
 STEP("configure-trace");
 if(trace_path){DWORD count=GetFullPathNameW(trace_path,32768,path,NULL);if(!count||count>=32768)FAIL(ERROR_FILENAME_EXCED_RANGE);REQUIRE(SetEnvironmentVariableW(L"BGI_TRANSLATION_TRACE",path));}
 else SetEnvironmentVariableW(L"BGI_TRANSLATION_TRACE",NULL);
 STEP("locate-original-executable");
 if(target){DWORD count=GetFullPathNameW(target,32768,exe,NULL);if(!count||count>=32768)FAIL(ERROR_FILENAME_EXCED_RANGE);}
 else{DWORD count=GetModuleFileNameW(NULL,exe,32768);if(!count||count>=32768)FAIL(ERROR_FILENAME_EXCED_RANGE);wchar_t*slash=wcsrchr(exe,L'\\');if(!slash)FAIL(ERROR_BAD_PATHNAME);*slash=0;
  if(wcslen(exe)+wcslen(game_exe)+2>=32768)FAIL(ERROR_FILENAME_EXCED_RANGE);
  wcscat(exe,L"\\");wcscat(exe,game_exe);
 }
 wcscpy(root,exe);wchar_t*slash=wcsrchr(root,L'\\');if(!slash)FAIL(ERROR_BAD_PATHNAME);*slash=0;
 STEP("verify-original-executable-sha256");REQUIRE(hash_file(exe,exe_sha));
 for(unsigned i=0;i<ARCHIVE_COUNT;i++){
  char message[128];snprintf(message,sizeof message,"verify-original-archive-sha256 index=%u",i);report(message);
  STEP("verify-original-archive-sha256");if(swprintf(path,32768,L"%ls\\Archive\\%ls",root,archives[i].name)<0)FAIL(ERROR_FILENAME_EXCED_RANGE);REQUIRE(hash_file(path,archives[i].sha));
 }
 STEP("create-temporary-directory");DWORD count=GetTempPathW(32768,temp);if(!count||count>=32600)FAIL(ERROR_FILENAME_EXCED_RANGE);
 REQUIRE(GetTempFileNameW(temp,L"bgt",0,stage));REQUIRE(DeleteFileW(stage));REQUIRE(CreateDirectoryW(stage,NULL));created=TRUE;
 STEP("extract-archive-overlays");
 for(unsigned i=0;i<ARCHIVE_COUNT;i++){if(swprintf(path,32768,L"%ls\\%ls",stage,archives[i].name)<0)FAIL(ERROR_FILENAME_EXCED_RANGE);REQUIRE(extract(100+i,path));extracted++;}
 STEP("extract-loader-dll");if(swprintf(dll,32768,L"%ls\\translation-loader.dll",stage)<0)FAIL(ERROR_FILENAME_EXCED_RANGE);REQUIRE(extract(10,dll));
 STEP("create-ready-event");swprintf(event_name,128,L"Local\\BGITranslationReady-%lu",GetCurrentProcessId());event=CreateEventW(NULL,TRUE,FALSE,event_name);REQUIRE(event);
 STEP("configure-child-environment");
 wcscpy(path,diagnostic_path);wchar_t*log_name=wcsrchr(path,L'\\');if(log_name)wcscpy(log_name+1,L"BGI_Chinese-hook.log");
 REQUIRE(SetEnvironmentVariableW(L"BGI_TRANSLATION_DLL_DIAGNOSTIC",path));
 REQUIRE(SetEnvironmentVariableW(L"BGI_TRANSLATION_ROOT",root));REQUIRE(SetEnvironmentVariableW(L"BGI_TRANSLATION_STAGE",stage));REQUIRE(SetEnvironmentVariableW(L"BGI_TRANSLATION_READY",event_name));
 STEP("create-original-process-suspended");if(swprintf(command,32768,L"\"%ls\"",exe)<0)FAIL(ERROR_FILENAME_EXCED_RANGE);
 STARTUPINFOW startup={.cb=sizeof startup};REQUIRE(CreateProcessW(exe,command,NULL,NULL,FALSE,CREATE_SUSPENDED,NULL,root,&startup,&process));
 STEP("resolve-remote-ntdll-LdrLoadDll");void*load=remote_export(process.hProcess,process.dwProcessId,L"ntdll.dll","LdrLoadDll");REQUIRE(load);
 STEP("allocate-remote-bootstrap");SIZE_T bytes=(wcslen(dll)+1)*sizeof(wchar_t),size=4096+sizeof context+bytes;
 remote=VirtualAllocEx(process.hProcess,NULL,size,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);REQUIRE(remote);
 context.function=(uint64_t)(uintptr_t)load;context.length=(USHORT)(bytes-sizeof(wchar_t));context.maximum_length=(USHORT)bytes;
 context.buffer=(uint64_t)(uintptr_t)remote+4096+sizeof context;context.status=(LONG)0xc0000001;
 if(bytes>65534)FAIL(ERROR_FILENAME_EXCED_RANGE);
 STEP("write-remote-bootstrap");REQUIRE(WriteProcessMemory(process.hProcess,remote,bootstrap_code,sizeof bootstrap_code,&written)&&written==sizeof bootstrap_code);
 REQUIRE(WriteProcessMemory(process.hProcess,(char*)remote+4096,&context,sizeof context,&written)&&written==sizeof context);
 REQUIRE(WriteProcessMemory(process.hProcess,(void*)(uintptr_t)context.buffer,dll,bytes,&written)&&written==bytes);
 STEP("protect-remote-bootstrap");DWORD previous;REQUIRE(VirtualProtectEx(process.hProcess,remote,4096,PAGE_EXECUTE_READ,&previous));REQUIRE(FlushInstructionCache(process.hProcess,remote,sizeof bootstrap_code));
 STEP("start-remote-bootstrap");injected=CreateRemoteThread(process.hProcess,NULL,0,(void*)remote,(char*)remote+4096,0,NULL);REQUIRE(injected);
 STEP("wait-remote-bootstrap");DWORD wait=WaitForSingleObject(injected,20000);if(wait==WAIT_TIMEOUT)FAIL(WAIT_TIMEOUT);REQUIRE(wait==WAIT_OBJECT_0);
 REQUIRE(GetExitCodeThread(injected,&thread_exit));
 STEP("read-remote-loader-result");REQUIRE(ReadProcessMemory(process.hProcess,(char*)remote+4096,&context,sizeof context,&written)&&written==sizeof context);
 native_status=context.status;
 {char message[160];snprintf(message,sizeof message,"LdrLoadDll NTSTATUS=0x%08lx module_nonzero=%u bootstrap_exit=0x%08lx",(unsigned long)native_status,context.module!=0,(unsigned long)thread_exit);report(message);}
 if(native_status<0||!context.module)FAIL(ERROR_DLL_INIT_FAILED);
 STEP("wait-overlay-ready-event");wait=WaitForSingleObject(event,0);if(wait!=WAIT_OBJECT_0)FAIL(wait==WAIT_TIMEOUT?ERROR_DLL_INIT_FAILED:GetLastError());
 VirtualFreeEx(process.hProcess,remote,0,MEM_RELEASE);remote=NULL;
 STEP("resume-original-process");REQUIRE(ResumeThread(process.hThread)!=(DWORD)-1);
 report("Validated original executable and archives; translation overlay active.");
 STEP("wait-original-process");wait=WaitForSingleObject(process.hProcess,deadline?deadline*1000:INFINITE);process_wait=wait;
 {DWORD saved=GetLastError();char message[160];snprintf(message,sizeof message,"WaitForSingleObject(original) result=0x%08lx Win32=%lu",(unsigned long)wait,(unsigned long)saved);report(message);SetLastError(saved);}
 if(wait==WAIT_TIMEOUT&&deadline){REQUIRE(TerminateProcess(process.hProcess,0));WaitForSingleObject(process.hProcess,5000);report("Probe deadline; stopped owned child.");status=0;}
 else if(wait==WAIT_OBJECT_0){
  STEP("read-original-exit-code");REQUIRE(GetExitCodeProcess(process.hProcess,&child_exit));
  char message[160];snprintf(message,sizeof message,"GetExitCodeProcess(original) success=1 code=%lu (0x%08lx)",(unsigned long)child_exit,(unsigned long)child_exit);report(message);
  status=(int)child_exit;if(status){step="original-process-exited-nonzero";failure_error=ERROR_PROCESS_ABORTED;}
 }
 else REQUIRE(FALSE);
 done:
 if(status){
  char message[512];snprintf(message,sizeof message,"Translation loader failed: stage=%s Win32=%lu (0x%08lx) NTSTATUS=0x%08lx bootstrap_exit=0x%08lx child_exit=0x%08lx wait_result=0x%08lx",step,(unsigned long)failure_error,(unsigned long)failure_error,(unsigned long)native_status,(unsigned long)thread_exit,(unsigned long)child_exit,(unsigned long)process_wait);report(message);
  wchar_t display[1024],system[256]={0};FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,NULL,failure_error,0,system,256,NULL);
  swprintf(display,1024,L"Translation loader failed.\nStage: %hs\nWindows error: %lu (0x%08lx)\nNTSTATUS: 0x%08lx\n%ls\nLog: %ls",step,(unsigned long)failure_error,(unsigned long)failure_error,(unsigned long)native_status,system,diagnostic_path);
  if(!deadline)MessageBoxW(NULL,display,L"BGI Chinese loader",MB_OK|MB_ICONERROR);
 }
 if(process.hProcess){if(WaitForSingleObject(process.hProcess,0)==WAIT_TIMEOUT){TerminateProcess(process.hProcess,1);WaitForSingleObject(process.hProcess,5000);}CloseHandle(process.hProcess);}
 if(process.hThread)CloseHandle(process.hThread);
 if(injected)CloseHandle(injected);
 if(event)CloseHandle(event);
 if(created){for(unsigned i=0;i<extracted;i++){swprintf(path,32768,L"%ls\\%ls",stage,archives[i].name);DeleteFileW(path);}swprintf(path,32768,L"%ls\\translation-loader.dll",stage);DeleteFileW(path);RemoveDirectoryW(stage);}
 if(diagnostic!=INVALID_HANDLE_VALUE)CloseHandle(diagnostic);
 return status;
}
#endif
