#include "precomp.h"

#include "CConsoleHandoff.h"

#include "srvinit.h"

// Holds the wwinmain open until COM tells us there are no more server connections
wil::unique_event _exitEvent;

// Routine Description:
// - Called back when COM says there is nothing left for our server to do and we can tear down.
void _releaseNotifier() noexcept
{
    _exitEvent.SetEvent();
}

DWORD g_cTerminalHandoffRegistration = 0;

template<int RegClsType>
class DefaultOutOfProcModuleWithRegistrationFlag;

template<int RegClsType, typename ModuleT = DefaultOutOfProcModuleWithRegistrationFlag<RegClsType>>
class OutOfProcModuleWithRegistrationFlag : public Microsoft::WRL::Module<Microsoft::WRL::ModuleType::OutOfProc, ModuleT>
{
public:
    STDMETHOD(RegisterCOMObject)
    (_In_opt_z_ const wchar_t* serverName, _In_reads_(count) IID* clsids, _In_reads_(count) IClassFactory** factories, _Inout_updates_(count) DWORD* cookies, unsigned int count)
    {
        return Microsoft::WRL::Details::RegisterCOMObject<RegClsType>(serverName, clsids, factories, cookies, count);
    }
};

template<int RegClsType>
class DefaultOutOfProcModuleWithRegistrationFlag : public OutOfProcModuleWithRegistrationFlag<RegClsType, DefaultOutOfProcModuleWithRegistrationFlag<RegClsType>>
{
};

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    OutOfProcModuleWithRegistrationFlag<REGCLS_SINGLEUSE>::Create();
}

// Routine Description:
// - Performs registrations for our COM types and waits until COM tells us we're done being a server.
// Return Value:
// - S_OK or a suitable COM/RPC error from registration or ongoing execution.
HRESULT RunAsComServer() noexcept
try
{
    _exitEvent.create();


     // has to be because the rest of TerminalConnection is apartment threaded already
    // also is it really necessary if the rest of it already is?
    RETURN_IF_FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));

    // We could probably hold this in a static...
    auto classFactory = Make<SimpleClassFactory<CConsoleHandoff>>();
    RETURN_IF_NULL_ALLOC(classFactory);

    ComPtr<IUnknown> unk;
    RETURN_IF_FAILED(classFactory.As(&unk));

    RETURN_IF_FAILED(CoRegisterClassObject(__uuidof(CConsoleHandoff), unk.Get(), CLSCTX_LOCAL_SERVER, REGCLS_SINGLEUSE, &g_cTerminalHandoffRegistration));

   /* auto uninit = wil::CoInitializeEx(COINIT_APARTMENTTHREADED);
    auto& module = Module<OutOfProc>::Create(&_releaseNotifier);
    RETURN_IF_FAILED(module.RegisterObjects());*/

    _exitEvent.wait();

    //RETURN_IF_FAILED(module.UnregisterObjects());

    return S_OK;
}
CATCH_RETURN()

// Routine Description:
// - Helper to duplicate a handle to ourselves so we can keep holding onto it
//   after the caller frees the original one.
// Arguments:
// - in - Handle to duplicate
// - out - Where to place the duplicated value
// Return Value:
// - S_OK or Win32 error from `::DuplicateHandle`
HRESULT _duplicateHandle(const HANDLE in, HANDLE& out)
{
    RETURN_IF_WIN32_BOOL_FALSE(DuplicateHandle(GetCurrentProcess(), in, GetCurrentProcess(), &out, 0, FALSE, DUPLICATE_SAME_ACCESS));
    return S_OK;
}

// Routine Description:
// - Takes the incoming information from COM and and prepares a console hosting session in this process.
// Arguments:
// - server - Console driver server handle
// - inputEvent - Event already established that we signal when new input data is available in case the driver is waiting on us
// - in - The input handle originally given to the inbox conhost on startup
// - out - The output handle original given to the inbox conhost on startup
// - argString - Argument string given to the original inbox conhost on startup
// - msg - Portable attach message containing just enough descriptor payload to get us started in servicing it
HRESULT CConsoleHandoff::EstablishHandoff(HANDLE server,
                                          HANDLE inputEvent,
                                          HANDLE in,
                                          HANDLE out,
                                          wchar_t* argString,
                                          PCCONSOLE_PORTABLE_ATTACH_MSG msg)
try
{
    // Fill the descriptor portion of a fresh api message with the received data.
    // The descriptor portion is the "received" packet from the last ask of the driver.
    // The other portions are unnecessary as they track the other buffer state, error codes,
    // and the return portion of the api message.
    // We will re-retrieve the connect information (title, window state, etc.) when the
    // new console session begins servicing this.
    CONSOLE_API_MSG apiMsg;
    apiMsg.Descriptor.Identifier.HighPart = msg->IdHighPart;
    apiMsg.Descriptor.Identifier.LowPart = msg->IdLowPart;
    apiMsg.Descriptor.Process = msg->Process;
    apiMsg.Descriptor.Object = msg->Object;
    apiMsg.Descriptor.Function = msg->Function;
    apiMsg.Descriptor.InputSize = msg->InputSize;
    apiMsg.Descriptor.OutputSize = msg->OutputSize;

    // Duplicate the handles from what we received.
    // The contract with COM specifies that any HANDLEs we receive from the caller belong
    // to the caller and will be freed when we leave the scope of this method.
    // Making our own duplicate copy ensures they hang around in our lifetime.
    RETURN_IF_FAILED(_duplicateHandle(server, server));
    RETURN_IF_FAILED(_duplicateHandle(inputEvent, inputEvent));
    /*RETURN_IF_FAILED(_duplicateHandle(in, in));
    RETURN_IF_FAILED(_duplicateHandle(out, out));*/

    // Build a console arguments structure that contains all information on how the
    // original console was started.
    ConsoleArguments consoleArgs;
    //(argString, in, out);
    UNREFERENCED_PARAMETER(in);
    UNREFERENCED_PARAMETER(out);
    UNREFERENCED_PARAMETER(argString);

    // Now perform the handoff.
    RETURN_IF_FAILED(ConsoleEstablishHandoff(server, &consoleArgs, inputEvent, &apiMsg));

    return S_OK;
}
CATCH_RETURN();