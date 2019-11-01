

#include <KR3/main.h>
#include <KR3/wl/windows.h>
#include <KR3/js/js.h>
#include <KR3/fs/file.h>
#include <KR3/util/path.h>
#include <KR3/parser/jsonparser.h>
#include <KR3/data/map.h>
#include <KR3/data/set.h>
#include <KR3/io/selfbufferedstream.h>
#include <KR3/data/crypt.h>
#include <KR3/net/ipaddr.h>
#include <KRWin/handle.h>
#include <KRWin/hook.h>

// #include "ChakraDebugService.h"

 #define USE_EDGEMODE_JSRT
 #include <jsrt.h>

#include <WinSock2.h>

#include "jsctx.h"
#include "nativepointer.h"
#include "reverse.h"
#include "console.h"
#include "fs.h"
#include "nethook.h"
#include "funchook.h"

#pragma comment(lib, "chakrart.lib")


using namespace kr;

namespace
{
	//JsDebugService s_debug;
	//JsDebugProtocolHandler s_debugHandler;
	win::Module* s_module = win::Module::getModule(nullptr);
	hook::IATHookerList s_iatChakra(s_module, "chakra.dll");
	hook::IATHookerList s_iatWS2_32(s_module, "WS2_32.dll");
	JsPersistent s_onError;
	Map<Text, AText> s_uuidToPackPath;
	Set<Ipv4Address> s_banlist;
	Map<Text16, JsPersistent> s_modules;
	AText s_jsmain;
	AText16 s_npmprefix;
}

struct ConnectionInfo
{
	void draw() noexcept
	{
		//const data = conninfo.data[this.address];
		//if (data)
		//{
		//	if (data.packetsPerSecMax < 10)
		//	{
		//		delete conninfo.data[this.address];
		//	}
		//	else
		//	{
		//		delete data.packetsPerSec;
		//	}
		//}
	}
};

void loadPackageJson() noexcept
{
	try
	{
		Must<File> file = File::open(u"package.json");
		JsonParser parse((File*)file);
		parse.fields([&](JsonField& field) {
			field("main", &s_jsmain);
		});
	}
	catch (Error&)
	{
		ConsoleColorScope _color = FOREGROUND_RED | FOREGROUND_INTENSITY;
		cerr << "ChakraX: failed to load package.json" << endl;
	}
}
void catchException() noexcept
{
	JsValueRef exception;
	if (JsGetAndClearException(&exception) == JsNoError)
	{
		JsScope scope;

		JsRawData exceptionobj = (JsRawData)exception;
		JsValue onError = s_onError;
		if (onError.isEmpty() || onError.call(undefined, { exceptionobj }) != false)
		{
			TText16 message = exceptionobj.getProperty(u"stack").as<Text16>();
			JsRelease(exception, nullptr);
			
			ConsoleColorScope _color = FOREGROUND_RED | FOREGROUND_INTENSITY;
			cerr << toAcp(message) << endl;
		}
	}
}

JsValue require(Text16 modulename) throws(JsException)
{
	TSZ16 normed = path16.join(modulename);
	auto res = s_modules.insert(normed, JsPersistent());
	if (!res.second) return res.first->second;

	TSZ16 filename;
	TText source;
	try
	{
		filename << modulename << u".js" << nullterm;
		source = File::openAsArray<char>(filename.data());
	}
	catch (Error&)
	{
		try
		{
			filename.clear();
			filename << s_npmprefix << modulename << u".js" << nullterm;
			source = File::openAsArray<char>(filename.data());
		}
		catch (Error&)
		{
			throw JsException(TSZ16() << u"module not found: " << modulename);
		}
	}

	JsValue exports = JsNewObject;
	JsValue func = JsRuntime::run(filename, TSZ16() << u"(exports)=>{" << utf8ToUtf16(source) << u"}", g_hookf->makeScriptId());
	func.call(undefined, { exports });

	res.first->second = exports;
	return exports;
}

JsErrorCode CALLBACK JsCreateRuntimeHook(
	JsRuntimeAttributes attributes,
	JsThreadServiceCallback threadService,
	JsRuntimeHandle* runtime) noexcept
{
	JsErrorCode err = JsCreateRuntime(attributes, threadService, runtime);
	if (err == JsNoError)
	{
		JsRuntime::setRuntime(*runtime);

		JsonParser parser(File::open(u"valid_known_packs.json"));

		parser.array([&](size_t idx){
			AText uuid;
			AText path;
			parser.fields([&](JsonField&field){
				field("uuid", &uuid);
				field("path", &path);
			});
			if (path == nullptr) return;
			if (uuid == nullptr) return;

			s_uuidToPackPath[uuid] = move(path);
		});
		
		//JsDebugServiceCreate(&s_debug);
		//JsDebugProtocolHandlerCreate(*runtime, &s_debugHandler);
		//JsDebugServiceRegisterHandler(s_debug, "minecraft", s_debugHandler, false);
		//JsDebugServiceListen(s_debug, 9229);
		//JsDebugProtocolHandlerWaitForDebugger(s_debugHandler);
	}
	return err;
}
JsErrorCode CALLBACK JsDisposeRuntimeHook(JsRuntimeHandle runtime) noexcept
{
	destroyNetHookModule();
	s_onError = JsPersistent();
	destroyJsContext();
	s_modules.clear();
	return JsDisposeRuntime(runtime);
}
JsErrorCode CALLBACK JsCreateContextHook(JsRuntimeHandle runtime, JsContextRef* newContext) noexcept
{
	JsErrorCode err = JsCreateContext(runtime, newContext);
	if (err == JsNoError)
	{
		createJsContext(*newContext);
		g_ctx->enter();

		JsValue chakraX = JsNewObject;
		chakraX.set(u"console", createConsoleModule());
		chakraX.setMethod(u"setOnErrorListener", [](JsValue listener){
			if (listener.getType() != JsType::Function) throw JsException(u"argument must be function");
			s_onError = listener;
		});
		chakraX.setMethod(u"execSync", [](Text16 path, JsValue curdir){
			return (AText)shell(path, curdir != undefined ? curdir.toString().as<Text16>().data() : nullptr );
		});
		chakraX.setMethod(u"debug", [] {
			requestDebugger();
			debug();
		});
		chakraX.set(u"fs", createFsModule());
		chakraX.set(u"NativePointer", NativePointer::classObject);
		chakraX.set(u"NativeFile", NativeFile::classObject);
		chakraX.set(u"nethook", createNetHookModule());

		{
			JsValue ipban = JsNewObject;
			ipban.setMethod(u"add", [](Text16 ipport) {
				Text16 iptext = ipport.readwith_e('|');
				if (iptext.empty()) return;
				s_banlist.insert(Ipv4Address(TSZ() << toNone(iptext)));
				});
			ipban.setMethod(u"remove", [](Text16 ipport) {
				Text16 iptext = ipport.readwith_e('|');
				if (iptext.empty()) return;
				s_banlist.erase(Ipv4Address(TSZ() << toNone(iptext)));
				});
			chakraX.set(u"ipban", ipban);
		}

		JsValue global = JsRuntime::global();
		global.set(u"chakraX", chakraX);
		global.setMethod(u"require", require);

		g_ctx->exit();
	}
	return err;
}
JsErrorCode CALLBACK JsRunScriptHook(
	const wchar_t* script,
	JsSourceContext sourceContext,
	const wchar_t* sourceUrl,
	JsValueRef* result) noexcept
{
	constexpr size_t UUID_LEN = 36;
	auto path = (Text16)unwide(sourceUrl);
	if (path.size() >= UUID_LEN)
	{
		TText uuid = TText::concat(toUtf8(path.cut(UUID_LEN)));
		auto iter = s_uuidToPackPath.find(uuid);
		if (iter != s_uuidToPackPath.end())
		{
			path.subarr_self(UUID_LEN);
			Text16 rpath = path.subarr(path.find_e(u'/'));
			pcstr16 remove_end = rpath.find_r(u'_');
			if (remove_end != nullptr) rpath.cut_self(remove_end);

			TText16 newpath = TText16::concat(utf8ToUtf16(iter->second), rpath, nullterm);
			JsErrorCode err = JsRunScript(script, sourceContext, wide(newpath.data()), result);
			if (err != JsNoError) catchException();
			return err;
		}
	}
	JsErrorCode err = JsRunScript(script, sourceContext, sourceUrl, result);
	if (err != JsNoError) catchException();
	return err;
}
JsErrorCode CALLBACK JsCallFunctionHook(
	JsValueRef function,
	JsValueRef* arguments,
	unsigned short argumentCount,
	JsValueRef* result) noexcept
{
	JsErrorCode err = JsCallFunction(function, arguments, argumentCount, result);
	if (err != JsNoError) catchException();
	return err;
}


int CALLBACK recvfromHook(
	SOCKET s, char* buf, int len, int flags, 
	sockaddr* from, int* fromlen
)
{
	int res = recvfrom(s, buf, len, flags, from, fromlen);
	if (s_banlist.find(((Ipv4Address&)((sockaddr_in*)from)->sin_addr)) != s_banlist.end())
	{
		*fromlen = 0;
		WSASetLastError(WSAECONNREFUSED);
		return -1;
	}
	return res;
}

BOOL WINAPI DllMain(
	_In_ HINSTANCE hinstDLL,
	_In_ DWORD     fdwReason,
	_In_ LPVOID    lpvReserved
)
{
	if (fdwReason == DLL_PROCESS_ATTACH)
	{
		ondebug(requestDebugger());
				
		cout << "ChakraX: Attached" << endl;
		{
			TText npmprefix = shell(u"npm config get prefix");
			s_npmprefix = (AnsiToUtf16)npmprefix.cut(npmprefix.find_ry("\r\n"));
		}

		{
			ModuleName<char16> moduleName;
			BText<32> hash = (encoder::Hex)(TBuffer)encoder::Md5::hash(File::open(moduleName.c_str()));
			cout << "ChakraX: bedrock_server.exe MD5 = " << hash << endl;

			if (hash == "221D0A275BE0BBBD3E50365799111742")
			{
				cout << "ChakraX: Expected Version = 1.12.0.28" << endl;
				{
					ConsoleColorScope _color = FOREGROUND_RED | FOREGROUND_INTENSITY;
					cerr << "ChakraX: Not Supported";
				}
				g_mcf.load_1_12_0_28();
			}
			else if (hash == "91B89F3745A2F64139FC6A955EFAD225")
			{
				cout << "ChakraX: Expected Version = 1.12.1.1" << endl;
				{
					ConsoleColorScope _color = FOREGROUND_RED | FOREGROUND_INTENSITY;
					cerr << "ChakraX: Not Supported";
				}
				g_mcf.load_1_12_0_28();
			}
			else if (hash == "BF16F04AD1783591BC80D1D3E54625E7")
			{
				cout << "ChakraX: Expected Version = 1.13.0.34" << endl;
				g_mcf.load_1_13_0_34();
			}
			else
			{
				{
					ConsoleColorScope _color = FOREGROUND_RED | FOREGROUND_INTENSITY;
					cerr << "ChakraX: Unexpected Version" << endl;
				}
				g_mcf.loadFromPdb();
			}
		}

		loadPackageJson();

		g_hookf->hookOnLoopStart([](DedicatedServer* server, ServerInstance * instance) {
			g_server = server;
			g_serverInstance = instance;
		});
		g_hookf->hookOnScriptLoading([]{
			// create require
			if (s_jsmain != nullptr)
			{
				TSZ16 main16;
				main16 << (Utf8ToUtf16)s_jsmain;
				s_jsmain = nullptr;
				require(main16);
			}
		});
		s_iatChakra.hooking("JsCreateContext", JsCreateContextHook);
		s_iatChakra.hooking("JsCreateRuntime", JsCreateRuntimeHook);
		s_iatChakra.hooking("JsDisposeRuntime", JsDisposeRuntimeHook);
		s_iatChakra.hooking("JsRunScript", JsRunScriptHook);
		s_iatChakra.hooking("JsCallFunction", JsCallFunctionHook);
		s_iatWS2_32.hooking(17, recvfromHook); // recvfrom
	}
	return true;
}

