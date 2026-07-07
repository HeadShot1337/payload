unit UnitRemoteMonitoring;

interface

uses
  Winapi.Windows, Winapi.Messages, Winapi.ActiveX,
  System.SysUtils, System.Classes, System.JSON, System.Math,
  System.NetEncoding, System.StrUtils, System.SyncObjs,
  Vcl.Graphics, Vcl.Controls, Vcl.Forms, Vcl.Dialogs, Vcl.StdCtrls,
  Vcl.ComCtrls, Vcl.ExtCtrls, Vcl.Imaging.jpeg, Vcl.Imaging.pngimage,
  ncLines;

{ -------------------------------------------------------------------------
  Format constants — must match ServerManager.pas and the C++ plugin.
  ------------------------------------------------------------------------- }
const
  MONITOR_FRAME_FORMAT_JPEG = 1;
  MONITOR_FRAME_FORMAT_H264 = 2;
  MONITOR_FRAME_FORMAT_H265 = 3;

type
  TMonitoringSendJSONEvent   = procedure(aLine: TncLine; JSONObj: TJSONObject) of object;
  TMonitoringFormClosedEvent = procedure(aLine: TncLine) of object;

  // Flicker'siz PaintBox: WM_ERASEBKGND'yi engeller
  TNoFlickerPaintBox = class(TPaintBox)
  protected
    procedure WMEraseBkgnd(var Msg: TWMEraseBkgnd); message WM_ERASEBKGND;
  end;

{ -------------------------------------------------------------------------
  H.264 decoder wrapper — uses Windows Media Foundation IMFTransform.
  The object is created once per TForm6 instance and reused across frames
  to avoid the expensive per-frame CoCreateInstance cost.
  ------------------------------------------------------------------------- }
  TH264Decoder = class
  private
    FDecoder      : Pointer;   { IMFTransform* — kept as untyped to avoid
                                 MF headers in the Delphi unit's interface  }
    FWidth        : Integer;
    FHeight       : Integer;
    FInitialized  : Boolean;
    FTimestamp    : Int64;     { 100-ns units }
    FFrameCount   : Integer;
    FOutputBufferSize: DWORD;

    function  TryInit(AWidth, AHeight: Integer): Boolean;
  public
    constructor Create;
    destructor  Destroy; override;

    { Decode one H.264 Annex-B NAL packet → TBitmap (pf24bit).
      Returns nil on failure or when the decoder has buffered but not yet
      produced a frame (normal for the first few input frames).            }
    function DecodeFrame(const ABytes: TBytes;
                         AWidth, AHeight: Integer): TBitmap;

    procedure Reset;
  end;

  TH265Decoder = class
  private
    FDecoder      : Pointer;
    FWidth        : Integer;
    FHeight       : Integer;
    FInitialized  : Boolean;
    FTimestamp    : Int64;
    FFrameCount   : Integer;
    FOutputBufferSize: DWORD;
    function TryInit(AWidth, AHeight: Integer): Boolean;
  public
    constructor Create;
    destructor Destroy; override;
    function DecodeFrame(const ABytes: TBytes; AWidth, AHeight: Integer): TBitmap;
    procedure Reset;
  end;

type
  TForm6 = class(TForm)
    StatusBar1: TStatusBar;
    Panel1: TPanel;
    Button1: TButton;
    ComboBox1: TComboBox;
    CheckBox1: TCheckBox;
    CheckBox2: TCheckBox;
    ComboBox2: TComboBox;
    PaintBox1: TPaintBox;          // DFM'de kalır ama gizlenecek
    procedure Button1Click(Sender: TObject);
    procedure ComboBox1Change(Sender: TObject);
    procedure ComboBox2Change(Sender: TObject);
  private
    FLine             : TncLine;
    FClientID         : string;
    FOnSendJSON       : TMonitoringSendJSONEvent;
    FOnFormClosed     : TMonitoringFormClosedEvent;
    FCapturing        : Boolean;
    FLastFrameSize    : Integer;
    FLastStatusTick   : UInt64;
    FFrameTimer       : TTimer;
    FPendingFrame     : string;
    FPendingFrameBytes: TBytes;
    FPendingFrameFormat: Integer;   { MONITOR_FRAME_FORMAT_JPEG / H264 }
    FPendingFrameWidth : Integer;
    FPendingFrameHeight: Integer;
    FFrameLock        : TCriticalSection;
    FDecodeEvent      : TEvent;
    FDecodeThread     : TThread;
    FDecodeStopping   : Boolean;
    FDecodedBitmap    : TBitmap;
    FDecodedFrameSize : Integer;
    FDisplayBitmap    : TBitmap;           // Repaint fallback için
    FPaintBox         : TNoFlickerPaintBox; // Gerçek görüntü alanı
    FLastMouseMoveTick: UInt64;
    FH264Decoder      : TH264Decoder;     // H.264 decoder context (reused)
    FH265Decoder      : TH265Decoder;     // H.265 decoder context (reused)
    FCurrentCodec     : string;            // 'h264', 'h265' or 'jpeg' — shown in statusbar

    procedure FillDefaultOptions;
    procedure SendMonitoringCommand(const AAction: string);
    function  SelectedMonitorIndex: Integer;
    function  SelectedScalePercent: Integer;
    function  JSONValueText(JSONObj: TJSONObject; const AName: string): string;
    function  DecodeBase64Image(const AText: string; out ABytes: TBytes): Boolean;
    function  DecodeFrameToBitmap(const ABytes: TBytes;
                                  AFormat: Integer;
                                  out ABitmap: TBitmap): Boolean;
    procedure QueueFrame(const AText: string);
    procedure FrameTimerTimer(Sender: TObject);
    procedure PaintBoxPaint(Sender: TObject);
    procedure PaintFrameBitmap(ABitmap: TBitmap; AFrameSize: Integer);
    procedure StartFrameWorker;
    procedure StopFrameWorker;
    procedure DecodeFrameWorker;
    function  TakePendingFrame(out AText: string;
                               out ABytes: TBytes;
                               out AFormat: Integer): Boolean;
    function  TakeDecodedFrame(out ABitmap: TBitmap; out AFrameSize: Integer): Boolean;
    procedure UpdateStatusBar;
    procedure UpdateButtonCaption;
    procedure UpdateMonitorList(JSONObj: TJSONObject);

    procedure FPaintBoxMouseDown(Sender: TObject; Button: TMouseButton; Shift: TShiftState; X, Y: Integer);
    procedure FPaintBoxMouseMove(Sender: TObject; Shift: TShiftState; X, Y: Integer);
    procedure FPaintBoxMouseUp(Sender: TObject; Button: TMouseButton; Shift: TShiftState; X, Y: Integer);
    procedure FormKeyDown(Sender: TObject; var Key: Word; Shift: TShiftState);
    procedure FormKeyUp(Sender: TObject; var Key: Word; Shift: TShiftState);
  protected
    procedure DoClose(var Action: TCloseAction); override;
  public
    destructor Destroy; override;

    procedure SetupForClient(aLine: TncLine; const AClientID: string;
      ASendJSON: TMonitoringSendJSONEvent; AFormClosed: TMonitoringFormClosedEvent);
    procedure DetachCallbacks;
    procedure RequestMonitorList;
    procedure RequestCaptureStart;
    procedure RequestCaptureStop;
    { AFormat: MONITOR_FRAME_FORMAT_JPEG or MONITOR_FRAME_FORMAT_H264.
      AWidth/AHeight: encoded frame dimensions (needed by H.264 decoder).  }
    procedure QueueFrameBytes(const ABytes: TBytes;
                              AFormat: Integer = MONITOR_FRAME_FORMAT_JPEG;
                              AWidth: Integer  = 0;
                              AHeight: Integer = 0);
    procedure HandleMonitoringJSON(JSONObj: TJSONObject);
  end;

var
  Form6: TForm6;

implementation

{$R *.dfm}

{ =========================================================================
  Windows Media Foundation — minimal import declarations
  We use dynamic loading to avoid a hard dependency on mf*.lib from Delphi.
  ========================================================================= }

{ GUIDs for MF types we need }
const
  MFMediaType_Video_GUID : TGUID = '{73646976-0000-0010-8000-00AA00389B71}';
  MFVideoFormat_H264_GUID: TGUID = '{34363248-0000-0010-8000-00AA00389B71}'; { 'H264' }
  MFVideoFormat_HEVC_GUID: TGUID = '{35363248-0000-0010-8000-00AA00389B71}'; { 'HEVC' }
  MFVideoFormat_RGB32_GUID: TGUID = '{E436EB7E-524F-11CE-9F53-0020AF0BA770}';
  MFVideoFormat_NV12_GUID : TGUID = '{3231564E-0000-0010-8000-00AA00389B71}'; { 'NV12' }
  CLSID_CMSH264DecoderMFT : TGUID = '{62CE7E72-4C71-4D20-B15D-452831A87D9D}';
  CLSID_CMSH265DecoderMFT : TGUID = '{42453111-18EE-4304-8319-1AA4D0A5647C}';

  MF_MT_MAJOR_TYPE    : TGUID = '{48eba18e-f8c9-4687-bf11-0a74c9f96a8f}';
  MF_MT_SUBTYPE       : TGUID = '{f7e34c9a-42e8-4714-b74b-cb29d72c35e5}';
  MF_MT_FRAME_SIZE    : TGUID = '{1652c33d-d6b2-4012-b834-72030849a37d}';
  MF_MT_FRAME_RATE    : TGUID = '{c459a2e8-3d2c-4e44-b132-fee5156c7bb0}';
  MF_MT_INTERLACE_MODE: TGUID = '{e2724bb8-e676-4806-b4b2-a8d6efb44ccd}';

  MFVideoInterlace_Progressive = 2;

  MFT_MESSAGE_COMMAND_FLUSH          = $00000000;
  MFT_MESSAGE_NOTIFY_BEGIN_STREAMING = $10000001;
  MFT_MESSAGE_NOTIFY_START_OF_STREAM = $10000002;

  MF_E_TRANSFORM_NEED_MORE_INPUT = HRESULT($C00D6D72);
  MF_E_TRANSFORM_STREAM_CHANGE   = HRESULT($C00D6D73);
  S_OK   = 0;
  S_FALSE = 1;

type
  { Minimal COM interfaces — only the methods we call }
  IMFAttributes = interface(IUnknown)
    ['{2CD2D921-C447-44A7-A13C-4ADABFC247E3}']
    function GetItem(const guidKey: TGUID; var pValue: Pointer): HRESULT; stdcall;
    function GetItemType(const guidKey: TGUID; out pType: DWORD): HRESULT; stdcall;
    function CompareItem(const guidKey: TGUID; const Value: Pointer): HRESULT; stdcall;
    function Compare(pTheirs: IMFAttributes; MatchType: DWORD; out pbResult: BOOL): HRESULT; stdcall;
    function GetUINT32(const guidKey: TGUID; out punValue: UINT32): HRESULT; stdcall;
    function GetUINT64(const guidKey: TGUID; out punValue: UINT64): HRESULT; stdcall;
    function GetDouble(const guidKey: TGUID; out pfValue: Double): HRESULT; stdcall;
    function GetGUID(const guidKey: TGUID; out pguidValue: TGUID): HRESULT; stdcall;
    function GetStringLength(const guidKey: TGUID; out pcchLength: UINT32): HRESULT; stdcall;
    function GetString(const guidKey: TGUID; pwszValue: LPWSTR; cchBufSize: UINT32; var pcchLength: UINT32): HRESULT; stdcall;
    function GetAllocatedString(const guidKey: TGUID; out ppwszValue: LPWSTR; out pcchLength: UINT32): HRESULT; stdcall;
    function GetBlobSize(const guidKey: TGUID; out pcbBlobSize: UINT32): HRESULT; stdcall;
    function GetBlob(const guidKey: TGUID; pBuf: PByte; cbBufSize: UINT32; var pcbBlobSize: UINT32): HRESULT; stdcall;
    function GetAllocatedBlob(const guidKey: TGUID; out ppBuf: PByte; out pcbSize: UINT32): HRESULT; stdcall;
    function GetUnknown(const guidKey: TGUID; const riid: TGUID; out ppv): HRESULT; stdcall;
    function SetItem(const guidKey: TGUID; const Value): HRESULT; stdcall;
    function DeleteItem(const guidKey: TGUID): HRESULT; stdcall;
    function DeleteAllItems: HRESULT; stdcall;
    function SetUINT32(const guidKey: TGUID; unValue: UINT32): HRESULT; stdcall;
    function SetUINT64(const guidKey: TGUID; unValue: UINT64): HRESULT; stdcall;
    function SetDouble(const guidKey: TGUID; fValue: Double): HRESULT; stdcall;
    function SetGUID(const guidKey: TGUID; const guidValue: TGUID): HRESULT; stdcall;
    function SetString(const guidKey: TGUID; pwszValue: LPCWSTR): HRESULT; stdcall;
    function SetBlob(const guidKey: TGUID; pBuf: PByte; cbBufSize: UINT32): HRESULT; stdcall;
    function SetUnknown(const guidKey: TGUID; pUnknown: IUnknown): HRESULT; stdcall;
    function LockStore: HRESULT; stdcall;
    function UnlockStore: HRESULT; stdcall;
    function GetCount(out pcItems: UINT32): HRESULT; stdcall;
    function GetItemByIndex(unIndex: UINT32; out pguidKey: TGUID; var pValue): HRESULT; stdcall;
    function CopyAllItems(pDest: IMFAttributes): HRESULT; stdcall;
  end;

  IMFMediaType = interface(IMFAttributes)
    ['{44AE0FA8-EA31-4109-8D2E-4CAE4997C555}']
    function GetMajorType(out pguidMajorType: TGUID): HRESULT; stdcall;
    function IsCompressedFormat(out pfCompressed: BOOL): HRESULT; stdcall;
    function IsEqual(pIMediaType: IMFMediaType; out pdwFlags: DWORD): HRESULT; stdcall;
    function GetRepresentation(guidRepresentation: TGUID; out ppvRepresentation: Pointer): HRESULT; stdcall;
    function FreeRepresentation(guidRepresentation: TGUID; pvRepresentation: Pointer): HRESULT; stdcall;
  end;

  IMFMediaBuffer = interface(IUnknown)
    ['{045FA593-8799-42B8-BC8D-8968C6453507}']
    function Lock(out ppbBuffer: PByte; out pcbMaxLength: DWORD; out pcbCurrentLength: DWORD): HRESULT; stdcall;
    function Unlock: HRESULT; stdcall;
    function GetCurrentLength(out pcbCurrentLength: DWORD): HRESULT; stdcall;
    function SetCurrentLength(cbCurrentLength: DWORD): HRESULT; stdcall;
    function GetMaxLength(out pcbMaxLength: DWORD): HRESULT; stdcall;
  end;

  IMFSample = interface(IMFAttributes)
    ['{C40A00F2-B93A-4D80-AE8C-5A1C634F58E4}']
    function GetSampleFlags(out pdwSampleFlags: DWORD): HRESULT; stdcall;
    function SetSampleFlags(dwSampleFlags: DWORD): HRESULT; stdcall;
    function GetSampleTime(out phnsSampleTime: Int64): HRESULT; stdcall;
    function SetSampleTime(hnsSampleTime: Int64): HRESULT; stdcall;
    function GetSampleDuration(out phnsSampleDuration: Int64): HRESULT; stdcall;
    function SetSampleDuration(hnsSampleDuration: Int64): HRESULT; stdcall;
    function GetBufferCount(out pdwBufferCount: DWORD): HRESULT; stdcall;
    function GetBufferByIndex(dwIndex: DWORD; out ppBuffer: IMFMediaBuffer): HRESULT; stdcall;
    function ConvertToContiguousBuffer(out ppBuffer: IMFMediaBuffer): HRESULT; stdcall;
    function AddBuffer(pBuffer: IMFMediaBuffer): HRESULT; stdcall;
    function RemoveBufferByIndex(dwIndex: DWORD): HRESULT; stdcall;
    function CopyToBuffer(pBuffer: IMFMediaBuffer): HRESULT; stdcall;
    function GetSampleSize(out pcbTotalLength: DWORD): HRESULT; stdcall;
    function IsContiguous(out pfIsContiguous: BOOL): HRESULT; stdcall;
    function SetContiguous(fIsContiguous: BOOL): HRESULT; stdcall;
  end;

  MFT_OUTPUT_DATA_BUFFER = record
    dwStreamID  : DWORD;
    pSample     : IMFSample;
    dwStatus    : DWORD;
    pEvents     : Pointer;
  end;

  TMFTOutputStreamInfo = record
    dwFlags     : DWORD;
    cbSize      : DWORD;
    cbAlignment : DWORD;
  end;

  IMFTransform = interface(IUnknown)
    ['{BF94C121-5B05-4E6F-8000-BA598961414D}']
    function GetStreamLimits(out pdwInputMinimum, pdwInputMaximum,
                              pdwOutputMinimum, pdwOutputMaximum: DWORD): HRESULT; stdcall;
    function GetStreamCount(out pcInputStreams, pcOutputStreams: DWORD): HRESULT; stdcall;
    function GetStreamIDs(dwInputIDArraySize: DWORD; pdwInputIDs: PDWORD;
                          dwOutputIDArraySize: DWORD; pdwOutputIDs: PDWORD): HRESULT; stdcall;
    function GetInputStreamInfo(dwInputStreamID: DWORD; out pStreamInfo): HRESULT; stdcall;
    function GetOutputStreamInfo(dwOutputStreamID: DWORD; out pStreamInfo): HRESULT; stdcall;
    function GetAttributes(out pAttributes: IMFAttributes): HRESULT; stdcall;
    function GetInputStreamAttributes(dwInputStreamID: DWORD; out pAttributes: IMFAttributes): HRESULT; stdcall;
    function GetOutputStreamAttributes(dwOutputStreamID: DWORD; out pAttributes: IMFAttributes): HRESULT; stdcall;
    function DeleteInputStream(dwStreamID: DWORD): HRESULT; stdcall;
    function AddInputStreams(cStreams: DWORD; adwStreamIDs: PDWORD): HRESULT; stdcall;
    function GetInputAvailableType(dwInputStreamID, dwTypeIndex: DWORD; out ppType: IMFMediaType): HRESULT; stdcall;
    function GetOutputAvailableType(dwOutputStreamID, dwTypeIndex: DWORD; out ppType: IMFMediaType): HRESULT; stdcall;
    function SetInputType(dwInputStreamID: DWORD; pType: IMFMediaType; dwFlags: DWORD): HRESULT; stdcall;
    function SetOutputType(dwOutputStreamID: DWORD; pType: IMFMediaType; dwFlags: DWORD): HRESULT; stdcall;
    function GetInputCurrentType(dwInputStreamID: DWORD; out ppType: IMFMediaType): HRESULT; stdcall;
    function GetOutputCurrentType(dwOutputStreamID: DWORD; out ppType: IMFMediaType): HRESULT; stdcall;
    function GetInputStatus(dwInputStreamID: DWORD; out pdwFlags: DWORD): HRESULT; stdcall;
    function GetOutputStatus(out pdwFlags: DWORD): HRESULT; stdcall;
    function SetOutputBounds(hnsLowerBound, hnsUpperBound: Int64): HRESULT; stdcall;
    function ProcessEvent(dwInputStreamID: DWORD; pEvent: IUnknown): HRESULT; stdcall;
    function ProcessMessage(eMessage: DWORD; ulParam: ULONG_PTR): HRESULT; stdcall;
    function ProcessInput(dwInputStreamID: DWORD; pSample: IMFSample; dwFlags: DWORD): HRESULT; stdcall;
    function ProcessOutput(dwFlags: DWORD; cOutputBufferCount: DWORD;
                           var pOutputSamples; out pdwStatus: DWORD): HRESULT; stdcall;
  end;

{ Dynamic MF function types }
type
  TFnMFStartup         = function(Version: ULONG; dwFlags: DWORD): HRESULT; stdcall;
  TFnMFShutdown        = function: HRESULT; stdcall;
  TFnMFCreateSample    = function(out ppIMFSample: IMFSample): HRESULT; stdcall;
  TFnMFCreateMemBuf    = function(cbMaxLength: DWORD; out ppBuffer: IMFMediaBuffer): HRESULT; stdcall;
  TFnMFCreateMediaType = function(out ppMFType: IMFMediaType): HRESULT; stdcall;
  TFnMFSetAttrSize     = function(pAttributes: IMFAttributes; const guidKey: TGUID;
                                  unWidth, unHeight: UINT32): HRESULT; stdcall;
  TFnMFSetAttrRatio    = function(pAttributes: IMFAttributes; const guidKey: TGUID;
                                  unNumerator, unDenominator: UINT32): HRESULT; stdcall;

var
  GHMFPlat      : HMODULE = 0;
  GHMFLib       : HMODULE = 0;
  GMFStarted    : Boolean = False;
  GMFLock       : TCriticalSection = nil;

  GMFStartup    : TFnMFStartup    = nil;
  GMFShutdown   : TFnMFShutdown   = nil;
  GMFCreateSample: TFnMFCreateSample = nil;
  GMFCreateMemBuf: TFnMFCreateMemBuf = nil;
  GMFCreateMT   : TFnMFCreateMediaType = nil;
  GMFSetSize    : TFnMFSetAttrSize  = nil;
  GMFSetRatio   : TFnMFSetAttrRatio = nil;

procedure EnsureMFLoaded;
begin
  if GMFLock = nil then
    GMFLock := TCriticalSection.Create;

  GMFLock.Enter;
  try
    if GMFStarted then Exit;

    GHMFPlat := LoadLibrary('mfplat.dll');
    GHMFLib  := LoadLibrary('mf.dll');
    if (GHMFPlat = 0) or (GHMFLib = 0) then Exit;

    GMFStartup     := GetProcAddress(GHMFPlat, 'MFStartup');
    GMFShutdown    := GetProcAddress(GHMFPlat, 'MFShutdown');
    GMFCreateSample:= GetProcAddress(GHMFPlat, 'MFCreateSample');
    GMFCreateMemBuf:= GetProcAddress(GHMFPlat, 'MFCreateMemoryBuffer');
    GMFCreateMT    := GetProcAddress(GHMFPlat, 'MFCreateMediaType');
    GMFSetSize     := GetProcAddress(GHMFPlat, 'MFSetAttributeSize');
    GMFSetRatio    := GetProcAddress(GHMFPlat, 'MFSetAttributeRatio');

    if Assigned(GMFStartup) then
    begin
      const MF_VERSION_DELPHI = $00020070; { MFAPI_VERSION }
      if SUCCEEDED(GMFStartup(MF_VERSION_DELPHI, 0)) then
        GMFStarted := True;
    end;
  finally
    GMFLock.Leave;
  end;
end;

{ =========================================================================
  NV12 → BGR24 conversion (for bitmap output from the H.264 decoder)
  RGB32 output from the decoder is easier; we request RGB32 and convert.
  ========================================================================= }

procedure NV12ToBitmap(const AData: PByte; AWidth, AHeight: Integer;
                        ABitmap: TBitmap);
var
  Row, Col : Integer;
  YVal     : Integer;
  UVal, VVal: Integer;
  R, G, B  : Integer;
  C, D, E  : Integer;
  YPtr     : PByte;
  UVPtr    : PByte;
  Dst      : PByte;
begin
  ABitmap.PixelFormat := pf24bit;
  ABitmap.SetSize(AWidth, AHeight);

  YPtr  := AData;
  UVPtr := AData + (AWidth * AHeight);

  for Row := 0 to AHeight - 1 do
  begin
    Dst := ABitmap.ScanLine[Row];
    for Col := 0 to AWidth - 1 do
    begin
      YVal := YPtr[Row * AWidth + Col];
      UVal := UVPtr[(Row div 2) * AWidth + (Col div 2) * 2];
      VVal := UVPtr[(Row div 2) * AWidth + (Col div 2) * 2 + 1];

      C := YVal - 16;
      D := UVal - 128;
      E := VVal - 128;

      R := (298 * C           + 409 * E + 128) div 256;
      G := (298 * C - 100 * D - 208 * E + 128) div 256;
      B := (298 * C + 516 * D           + 128) div 256;

      if R < 0 then R := 0 else if R > 255 then R := 255;
      if G < 0 then G := 0 else if G > 255 then G := 255;
      if B < 0 then B := 0 else if B > 255 then B := 255;

      Dst[0] := B;
      Dst[1] := G;
      Dst[2] := R;
      Inc(Dst, 3);
    end;
  end;
end;

{ =========================================================================
  TH264Decoder
  ========================================================================= }

constructor TH264Decoder.Create;
begin
  inherited;
  FDecoder          := nil;
  FInitialized      := False;
  FTimestamp        := 0;
  FFrameCount       := 0;
  FOutputBufferSize := 0;
end;

destructor TH264Decoder.Destroy;
begin
  Reset;
  inherited;
end;

procedure TH264Decoder.Reset;
var
  Xfrm: IMFTransform;
begin
  if FDecoder <> nil then
  begin
    Xfrm := IMFTransform(FDecoder);
    Xfrm.ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    Xfrm := nil;
    IUnknown(FDecoder)._Release;
    FDecoder     := nil;
  end;
  FWidth            := 0;
  FHeight           := 0;
  FInitialized      := False;
  FTimestamp        := 0;
  FFrameCount       := 0;
  FOutputBufferSize := 0;
end;

function TH264Decoder.TryInit(AWidth, AHeight: Integer): Boolean;
var
  Xfrm      : IMFTransform;
  InType    : IMFMediaType;
  OutType   : IMFMediaType;
  hr        : HRESULT;
  StreamInfo: TMFTOutputStreamInfo;
begin
  Result := False;

  { Align to 2 — H.264 requires even dimensions }
  AWidth  := (AWidth  + 1) and not 1;
  AHeight := (AHeight + 1) and not 1;

  if FInitialized and (FWidth = AWidth) and (FHeight = AHeight) then
  begin
    Result := True;
    Exit;
  end;

  Reset;

  if not GMFStarted then Exit;
  if not Assigned(GMFCreateMT) then Exit;

  { Create the H.264 decoder MFT }
  hr := CoCreateInstance(CLSID_CMSH264DecoderMFT, nil,
                          CLSCTX_INPROC_SERVER,
                          IMFTransform, Xfrm);
  if FAILED(hr) then Exit;

  { Input: H.264 compressed }
  GMFCreateMT(InType);
  InType.SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video_GUID);
  InType.SetGUID(MF_MT_SUBTYPE,    MFVideoFormat_H264_GUID);
  if Assigned(GMFSetSize) then
    GMFSetSize(InType, MF_MT_FRAME_SIZE, AWidth, AHeight);

  hr := Xfrm.SetInputType(0, InType, 0);
  InType := nil;
  if FAILED(hr) then Exit;

  { Output: NV12 (natively supported by the decoder MFT) }
  GMFCreateMT(OutType);
  OutType.SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video_GUID);
  OutType.SetGUID(MF_MT_SUBTYPE,    MFVideoFormat_NV12_GUID);
  if Assigned(GMFSetSize) then
    GMFSetSize(OutType, MF_MT_FRAME_SIZE, AWidth, AHeight);
  OutType.SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

  hr := Xfrm.SetOutputType(0, OutType, 0);
  OutType := nil;
  if FAILED(hr) then
  begin
    { Fallback to whatever is available }
    hr := Xfrm.GetOutputAvailableType(0, 0, OutType);
    if SUCCEEDED(hr) then
      hr := Xfrm.SetOutputType(0, OutType, 0);
    OutType := nil;
    if FAILED(hr) then Exit;
  end;

  Xfrm.ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  Xfrm.ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

  { Query output buffer size info }
  if SUCCEEDED(Xfrm.GetOutputStreamInfo(0, StreamInfo)) then
    FOutputBufferSize := StreamInfo.cbSize
  else
    FOutputBufferSize := (AWidth * AHeight * 3) div 2;

  { Stash a raw reference — addref is already done by the interface assignment }
  FDecoder     := Pointer(Xfrm);
  IUnknown(FDecoder)._AddRef;
  Xfrm := nil;

  FWidth       := AWidth;
  FHeight      := AHeight;
  FInitialized := True;
  Result       := True;
end;

{ =========================================================================
  TH265Decoder
  ========================================================================= }

constructor TH265Decoder.Create;
begin
  inherited;
  FDecoder          := nil;
  FInitialized      := False;
  FTimestamp        := 0;
  FFrameCount       := 0;
  FOutputBufferSize := 0;
end;

destructor TH265Decoder.Destroy;
begin
  Reset;
  inherited;
end;

procedure TH265Decoder.Reset;
var
  Xfrm: IMFTransform;
begin
  if FDecoder <> nil then
  begin
    Xfrm := IMFTransform(FDecoder);
    Xfrm.ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    Xfrm := nil;
    IUnknown(FDecoder)._Release;
    FDecoder     := nil;
  end;
  FWidth            := 0;
  FHeight           := 0;
  FInitialized      := False;
  FTimestamp        := 0;
  FFrameCount       := 0;
  FOutputBufferSize := 0;
end;

function TH265Decoder.TryInit(AWidth, AHeight: Integer): Boolean;
var
  Xfrm      : IMFTransform;
  InType    : IMFMediaType;
  OutType   : IMFMediaType;
  hr        : HRESULT;
  StreamInfo: TMFTOutputStreamInfo;
begin
  Result := False;
  AWidth  := (AWidth  + 1) and not 1;
  AHeight := (AHeight + 1) and not 1;

  if FInitialized and (FWidth = AWidth) and (FHeight = AHeight) then
  begin
    Result := True;
    Exit;
  end;

  Reset;
  if not GMFStarted then Exit;
  if not Assigned(GMFCreateMT) then Exit;

  hr := CoCreateInstance(CLSID_CMSH265DecoderMFT, nil,
                          CLSCTX_INPROC_SERVER,
                          IMFTransform, Xfrm);
  if FAILED(hr) then Exit;

  GMFCreateMT(InType);
  InType.SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video_GUID);
  InType.SetGUID(MF_MT_SUBTYPE,    MFVideoFormat_HEVC_GUID);
  if Assigned(GMFSetSize) then
    GMFSetSize(InType, MF_MT_FRAME_SIZE, AWidth, AHeight);

  hr := Xfrm.SetInputType(0, InType, 0);
  InType := nil;
  if FAILED(hr) then Exit;

  GMFCreateMT(OutType);
  OutType.SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video_GUID);
  OutType.SetGUID(MF_MT_SUBTYPE,    MFVideoFormat_NV12_GUID);
  if Assigned(GMFSetSize) then
    GMFSetSize(OutType, MF_MT_FRAME_SIZE, AWidth, AHeight);
  OutType.SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

  hr := Xfrm.SetOutputType(0, OutType, 0);
  OutType := nil;
  if FAILED(hr) then
  begin
    hr := Xfrm.GetOutputAvailableType(0, 0, OutType);
    if SUCCEEDED(hr) then
      hr := Xfrm.SetOutputType(0, OutType, 0);
    OutType := nil;
    if FAILED(hr) then Exit;
  end;

  Xfrm.ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  Xfrm.ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

  if SUCCEEDED(Xfrm.GetOutputStreamInfo(0, StreamInfo)) then
    FOutputBufferSize := StreamInfo.cbSize
  else
    FOutputBufferSize := (AWidth * AHeight * 3) div 2;

  FDecoder     := Pointer(Xfrm);
  IUnknown(FDecoder)._AddRef;
  Xfrm := nil;

  FWidth       := AWidth;
  FHeight      := AHeight;
  FInitialized := True;
  Result       := True;
end;

function TH265Decoder.DecodeFrame(const ABytes: TBytes;
                                   AWidth, AHeight: Integer): TBitmap;
var
  Xfrm     : IMFTransform;
  InBuf    : IMFMediaBuffer;
  InSmp    : IMFSample;
  OutBuf   : MFT_OUTPUT_DATA_BUFFER;
  pBuf     : PByte;
  Status   : DWORD;
  hr       : HRESULT;
  pData    : PByte;
  MaxLen   : DWORD;
  CurLen   : DWORD;
  OutSample: IMFSample;
  OutBuffer: IMFMediaBuffer;
  ContBuf  : IMFMediaBuffer;
  OutType  : IMFMediaType;
begin
  Result := nil;
  if Length(ABytes) = 0 then Exit;
  if not GMFStarted then Exit;
  if not Assigned(GMFCreateSample) or not Assigned(GMFCreateMemBuf) then Exit;
  if not TryInit(AWidth, AHeight) then Exit;

  Xfrm := IMFTransform(FDecoder);

  GMFCreateMemBuf(Length(ABytes), InBuf);
  InBuf.Lock(pBuf, MaxLen, CurLen);
  Move(ABytes[0], pBuf^, Length(ABytes));
  InBuf.Unlock;
  InBuf.SetCurrentLength(Length(ABytes));

  GMFCreateSample(InSmp);
  InSmp.AddBuffer(InBuf);
  InBuf := nil;

  const Duration100ns: Int64 = 333333;
  InSmp.SetSampleTime(FTimestamp);
  InSmp.SetSampleDuration(Duration100ns);
  FTimestamp := FTimestamp + Duration100ns;
  Inc(FFrameCount);

  hr := Xfrm.ProcessInput(0, InSmp, 0);
  InSmp := nil;
  if FAILED(hr) then Exit;

  { Pull output — handle multi-frame drain or stream changes }
  while True do
  begin
    GMFCreateSample(OutSample);
    GMFCreateMemBuf(FOutputBufferSize, OutBuffer);
    OutSample.AddBuffer(OutBuffer);
    OutBuffer := nil;

    FillChar(OutBuf, SizeOf(OutBuf), 0);
    OutBuf.dwStreamID := 0;
    OutBuf.pSample    := OutSample;
    Status := 0;

    hr := Xfrm.ProcessOutput(0, 1, OutBuf, Status);

    if hr = MF_E_TRANSFORM_STREAM_CHANGE then
    begin
      if OutSample <> nil then OutSample := nil;
      hr := Xfrm.GetOutputAvailableType(0, 0, OutType);
      if SUCCEEDED(hr) then
      begin
        Xfrm.SetOutputType(0, OutType, 0);
        OutType := nil;
      end;
      Continue;
    end;

    if hr = MF_E_TRANSFORM_NEED_MORE_INPUT then
    begin
      if OutSample <> nil then OutSample := nil;
      Break;
    end;

    if FAILED(hr) or (OutBuf.pSample = nil) then
    begin
      if OutSample <> nil then OutSample := nil;
      if OutBuf.pSample <> nil then OutBuf.pSample := nil;
      if OutBuf.pEvents <> nil then IUnknown(OutBuf.pEvents)._Release;
      Break;
    end;

    try
      OutBuf.pSample.ConvertToContiguousBuffer(ContBuf);
      if ContBuf <> nil then
      try
        ContBuf.Lock(pData, MaxLen, CurLen);
        try
          if CurLen >= Cardinal((FWidth * FHeight * 3) div 2) then
          begin
            if Result = nil then Result := TBitmap.Create;
            NV12ToBitmap(pData, FWidth, FHeight, Result);
          end;
        finally
          ContBuf.Unlock;
        end;
      finally
        ContBuf := nil;
      end;
    finally
      if OutBuf.pSample <> nil then OutBuf.pSample := nil;
      if OutSample <> nil then OutSample := nil;
      if OutBuf.pEvents <> nil then IUnknown(OutBuf.pEvents)._Release;
    end;
  end;
end;

function TH264Decoder.DecodeFrame(const ABytes: TBytes;
                                   AWidth, AHeight: Integer): TBitmap;
var
  Xfrm     : IMFTransform;
  InBuf    : IMFMediaBuffer;
  InSmp    : IMFSample;
  OutBuf   : MFT_OUTPUT_DATA_BUFFER;
  pBuf     : PByte;
  Status   : DWORD;
  hr       : HRESULT;
  pData    : PByte;
  MaxLen   : DWORD;
  CurLen   : DWORD;
  OutSample: IMFSample;
  OutBuffer: IMFMediaBuffer;
  ContBuf  : IMFMediaBuffer;
  OutType  : IMFMediaType;
begin
  Result := nil;
  if Length(ABytes) = 0 then Exit;
  if not GMFStarted then Exit;
  if not Assigned(GMFCreateSample) or not Assigned(GMFCreateMemBuf) then Exit;
  if not TryInit(AWidth, AHeight) then Exit;

  Xfrm := IMFTransform(FDecoder);

  GMFCreateMemBuf(Length(ABytes), InBuf);
  InBuf.Lock(pBuf, MaxLen, CurLen);
  Move(ABytes[0], pBuf^, Length(ABytes));
  InBuf.Unlock;
  InBuf.SetCurrentLength(Length(ABytes));

  GMFCreateSample(InSmp);
  InSmp.AddBuffer(InBuf);
  InBuf := nil;

  const Duration100ns: Int64 = 333333;
  InSmp.SetSampleTime(FTimestamp);
  InSmp.SetSampleDuration(Duration100ns);
  FTimestamp := FTimestamp + Duration100ns;
  Inc(FFrameCount);

  hr := Xfrm.ProcessInput(0, InSmp, 0);
  if FAILED(hr) then Exit;

  while True do
  begin
    GMFCreateSample(OutSample);
    GMFCreateMemBuf(FOutputBufferSize, OutBuffer);
    OutSample.AddBuffer(OutBuffer);
    OutBuffer := nil;

    FillChar(OutBuf, SizeOf(OutBuf), 0);
    OutBuf.dwStreamID := 0;
    OutBuf.pSample    := OutSample;
    Status := 0;

    hr := Xfrm.ProcessOutput(0, 1, OutBuf, Status);

    if hr = MF_E_TRANSFORM_STREAM_CHANGE then
    begin
      if OutSample <> nil then OutSample := nil;
      hr := Xfrm.GetOutputAvailableType(0, 0, OutType);
      if SUCCEEDED(hr) then
      begin
        Xfrm.SetOutputType(0, OutType, 0);
        OutType := nil;
      end;
      Continue;
    end;

    if hr = MF_E_TRANSFORM_NEED_MORE_INPUT then
    begin
      if OutSample <> nil then OutSample := nil;
      Break;
    end;

    if FAILED(hr) or (OutBuf.pSample = nil) then
    begin
      if OutSample <> nil then OutSample := nil;
      if OutBuf.pSample <> nil then OutBuf.pSample := nil;
      if OutBuf.pEvents <> nil then IUnknown(OutBuf.pEvents)._Release;
      Break;
    end;

    try
      OutBuf.pSample.ConvertToContiguousBuffer(ContBuf);
      if ContBuf <> nil then
      try
        ContBuf.Lock(pData, MaxLen, CurLen);
        try
          if CurLen >= Cardinal((FWidth * FHeight * 3) div 2) then
          begin
            if Result = nil then Result := TBitmap.Create;
            NV12ToBitmap(pData, FWidth, FHeight, Result);
          end;
        finally
          ContBuf.Unlock;
        end;
      finally
        ContBuf := nil;
      end;
    finally
      if OutBuf.pSample <> nil then OutBuf.pSample := nil;
      if OutSample <> nil then OutSample := nil;
      if OutBuf.pEvents <> nil then IUnknown(OutBuf.pEvents)._Release;
    end;
  end;
end;

{ =========================================================================
  TNoFlickerPaintBox
  ========================================================================= }

procedure TNoFlickerPaintBox.WMEraseBkgnd(var Msg: TWMEraseBkgnd);
begin
  // Arka plan silmeyi tamamen engelle → siyah flash yok
  Msg.Result := 1;
end;

{ =========================================================================
  TForm6
  ========================================================================= }

destructor TForm6.Destroy;
begin
  if FCapturing and Assigned(FOnSendJSON) and Assigned(FLine) then
    RequestCaptureStop;

  StopFrameWorker;

  if Assigned(FFrameTimer) then
    FFrameTimer.Enabled := False;

  if Assigned(FOnFormClosed) and Assigned(FLine) then
    FOnFormClosed(FLine);

  DetachCallbacks;
  FreeAndNil(FH264Decoder);
  FreeAndNil(FH265Decoder);
  FreeAndNil(FFrameTimer);
  FreeAndNil(FDecodedBitmap);
  FreeAndNil(FDisplayBitmap);
  FreeAndNil(FDecodeEvent);
  FreeAndNil(FFrameLock);
  // FPaintBox: TComponent.Owner'a bağlı, otomatik serbest bırakılır
  inherited;
end;

procedure TForm6.DetachCallbacks;
begin
  FOnSendJSON   := nil;
  FOnFormClosed := nil;
end;

procedure TForm6.DoClose(var Action: TCloseAction);
begin
  if FCapturing then
    RequestCaptureStop;

  StopFrameWorker;

  if Assigned(FFrameTimer) then
    FFrameTimer.Enabled := False;
  FPendingFrame := '';
  SetLength(FPendingFrameBytes, 0);

  if Assigned(FOnFormClosed) and Assigned(FLine) then
    FOnFormClosed(FLine);

  DetachCallbacks;
  if Form6 = Self then
    Form6 := nil;

  inherited;
  Action := caFree;
end;

procedure TForm6.SetupForClient(aLine: TncLine; const AClientID: string;
  ASendJSON: TMonitoringSendJSONEvent; AFormClosed: TMonitoringFormClosedEvent);
begin
  FLine              := aLine;
  FClientID          := AClientID;
  FOnSendJSON        := ASendJSON;
  FOnFormClosed      := AFormClosed;
  FCapturing         := False;
  FLastFrameSize     := 0;
  FLastStatusTick    := 0;
  FPendingFrame      := '';
  SetLength(FPendingFrameBytes, 0);
  FPendingFrameFormat := MONITOR_FRAME_FORMAT_JPEG;
  FDecodeStopping    := False;
  FDecodedFrameSize  := 0;
  FCurrentCodec      := 'jpeg';

  if not Assigned(FFrameLock) then
    FFrameLock := TCriticalSection.Create;
  if not Assigned(FDecodeEvent) then
    FDecodeEvent := TEvent.Create(nil, True, False, '');

  if not Assigned(FFrameTimer) then
  begin
    FFrameTimer          := TTimer.Create(Self);
    FFrameTimer.Enabled  := False;
    FFrameTimer.Interval := 33;
    FFrameTimer.OnTimer  := FrameTimerTimer;
  end;

  { H.264 decoder — created once, reused for all frames }
  if not Assigned(FH264Decoder) then
  begin
    EnsureMFLoaded;
    FH264Decoder := TH264Decoder.Create;
  end;

  if not Assigned(FH265Decoder) then
  begin
    EnsureMFLoaded;
    FH265Decoder := TH265Decoder.Create;
  end;

  StartFrameWorker;

  // FDisplayBitmap: OnPaint fallback için
  if not Assigned(FDisplayBitmap) then
  begin
    FDisplayBitmap             := TBitmap.Create;
    FDisplayBitmap.PixelFormat := pf24bit;
  end;

  // TNoFlickerPaintBox'ı DFM'deki PaintBox1'in yerine oluştur
  if not Assigned(FPaintBox) then
  begin
    PaintBox1.Visible := False;          // DFM bileşenini gizle

    FPaintBox          := TNoFlickerPaintBox.Create(Self);
    FPaintBox.Parent   := PaintBox1.Parent;
    FPaintBox.SetBounds(PaintBox1.Left, PaintBox1.Top,
                        PaintBox1.Width, PaintBox1.Height);
    FPaintBox.Align    := PaintBox1.Align;
    FPaintBox.Anchors  := PaintBox1.Anchors;
    FPaintBox.OnPaint  := PaintBoxPaint;

    FPaintBox.OnMouseDown := FPaintBoxMouseDown;
    FPaintBox.OnMouseMove := FPaintBoxMouseMove;
    FPaintBox.OnMouseUp   := FPaintBoxMouseUp;

    // Üst panel varsa double buffer
    if FPaintBox.Parent is TWinControl then
      TWinControl(FPaintBox.Parent).DoubleBuffered := True;
  end;

  Caption        := 'Remote Monitoring - ' + FClientID;
  DoubleBuffered := True;
  KeyPreview     := True;
  OnKeyDown      := FormKeyDown;
  OnKeyUp        := FormKeyUp;

  Button1.OnClick    := Button1Click;
  ComboBox1.OnChange := ComboBox1Change;
  ComboBox2.OnChange := ComboBox2Change;

  FillDefaultOptions;
  UpdateButtonCaption;
  UpdateStatusBar;
end;

procedure TForm6.FillDefaultOptions;
var
  Pct: Integer;
begin
  if ComboBox1.Items.Count = 0 then
  begin
    for Pct := 10 to 100 do
      if (Pct mod 10) = 0 then
        ComboBox1.Items.Add(IntToStr(Pct) + '%');
    ComboBox1.ItemIndex := ComboBox1.Items.IndexOf('50%');
    if ComboBox1.ItemIndex < 0 then
      ComboBox1.ItemIndex := 0;
  end;

  if ComboBox2.Items.Count = 0 then
  begin
    ComboBox2.Items.Add('Monitor 1');
    ComboBox2.ItemIndex := 0;
  end
  else if ComboBox2.ItemIndex < 0 then
    ComboBox2.ItemIndex := 0;
end;

function TForm6.SelectedScalePercent: Integer;
var
  Text: string;
begin
  Result := 50;
  Text   := Trim(StringReplace(ComboBox1.Text, '%', '', [rfReplaceAll]));
  if Text <> '' then
    Result := StrToIntDef(Text, Result);

  if Result < 10  then Result := 10;
  if Result > 100 then Result := 100;
end;

function TForm6.SelectedMonitorIndex: Integer;
begin
  Result := ComboBox2.ItemIndex;
  if Result < 0 then
    Result := 0;
end;

procedure TForm6.SendMonitoringCommand(const AAction: string);
var
  JSONObj: TJSONObject;
begin
  if not Assigned(FLine) or not Assigned(FOnSendJSON) then
    Exit;

  JSONObj := TJSONObject.Create;
  try
    JSONObj.AddPair('action',  AAction);
    JSONObj.AddPair('monitor', TJSONNumber.Create(SelectedMonitorIndex));
    JSONObj.AddPair('scale',   TJSONNumber.Create(SelectedScalePercent));
    FOnSendJSON(FLine, JSONObj);
  finally
    JSONObj.Free;
  end;
end;

procedure TForm6.RequestMonitorList;
begin
  SendMonitoringCommand('monitorlist');
end;

procedure TForm6.RequestCaptureStart;
begin
  FCapturing := True;
  if Assigned(FFrameTimer) then
    FFrameTimer.Enabled := True;
  UpdateButtonCaption;
  UpdateStatusBar;
  SendMonitoringCommand('monitorstart');
end;

procedure TForm6.RequestCaptureStop;
begin
  SendMonitoringCommand('monitorstop');
  FCapturing := False;
  if Assigned(FFrameLock) then
  begin
    FFrameLock.Enter;
    try
      FPendingFrame := '';
      SetLength(FPendingFrameBytes, 0);
      FreeAndNil(FDecodedBitmap);
      FDecodedFrameSize := 0;
      if Assigned(FDecodeEvent) then
        FDecodeEvent.ResetEvent;
    finally
      FFrameLock.Leave;
    end;
  end;
  { Reset the H.264/H.265 decoders so the next session starts clean }
  if Assigned(FH264Decoder) then
    FH264Decoder.Reset;
  if Assigned(FH265Decoder) then
    FH265Decoder.Reset;
  if Assigned(FFrameTimer) then
    FFrameTimer.Enabled := False;
  UpdateButtonCaption;
  UpdateStatusBar;
end;

procedure TForm6.Button1Click(Sender: TObject);
begin
  if FCapturing then
    RequestCaptureStop
  else
    RequestCaptureStart;
end;

procedure TForm6.ComboBox1Change(Sender: TObject);
begin
  if FCapturing then
    SendMonitoringCommand('monitorstart');
end;

procedure TForm6.ComboBox2Change(Sender: TObject);
begin
  if FCapturing then
    SendMonitoringCommand('monitorstart');
end;

function TForm6.JSONValueText(JSONObj: TJSONObject; const AName: string): string;
var
  Val: TJSONValue;
begin
  Result := '';
  if JSONObj = nil then Exit;
  Val := JSONObj.Values[AName];
  if Assigned(Val) then
    Result := Val.Value;
end;

{ -------------------------------------------------------------------------
  QueueFrame / QueueFrameBytes
  ------------------------------------------------------------------------- }

procedure TForm6.QueueFrame(const AText: string);
begin
  if AText = '' then Exit;
  if not Assigned(FFrameLock) then Exit;

  FFrameLock.Enter;
  try
    FPendingFrame       := AText;
    FPendingFrameFormat := MONITOR_FRAME_FORMAT_JPEG;
    SetLength(FPendingFrameBytes, 0);
    if Assigned(FDecodeEvent) then
      FDecodeEvent.SetEvent;
  finally
    FFrameLock.Leave;
  end;
end;

procedure TForm6.QueueFrameBytes(const ABytes: TBytes;
                                  AFormat: Integer = MONITOR_FRAME_FORMAT_JPEG;
                                  AWidth: Integer  = 0;
                                  AHeight: Integer = 0);
begin
  if Length(ABytes) = 0 then Exit;
  if not Assigned(FFrameLock) then Exit;

  FFrameLock.Enter;
  try
    FPendingFrame        := '';
    FPendingFrameBytes   := Copy(ABytes, 0, Length(ABytes));
    FPendingFrameFormat  := AFormat;
    FPendingFrameWidth   := AWidth;
    FPendingFrameHeight  := AHeight;
    if Assigned(FDecodeEvent) then
      FDecodeEvent.SetEvent;
  finally
    FFrameLock.Leave;
  end;
end;

{ -------------------------------------------------------------------------
  Frame timer (UI thread) — picks up decoded bitmaps
  ------------------------------------------------------------------------- }

procedure TForm6.FrameTimerTimer(Sender: TObject);
var
  Bitmap   : TBitmap;
  FrameSize: Integer;
begin
  if not FCapturing then
  begin
    if Assigned(FFrameTimer) then
      FFrameTimer.Enabled := False;
    Exit;
  end;

  if TakeDecodedFrame(Bitmap, FrameSize) then
  begin
    try
      PaintFrameBitmap(Bitmap, FrameSize);
    finally
      Bitmap.Free;
    end;
  end;

  if Assigned(FFrameTimer) then
    FFrameTimer.Enabled := FCapturing;
end;

{ -------------------------------------------------------------------------
  Paint
  ------------------------------------------------------------------------- }

procedure TForm6.PaintBoxPaint(Sender: TObject);
begin
  if not Assigned(FPaintBox) then Exit;

  if not Assigned(FDisplayBitmap) or
     (FDisplayBitmap.Width <= 0) or (FDisplayBitmap.Height <= 0) then
  begin
    FPaintBox.Canvas.Brush.Color := clBlack;
    FPaintBox.Canvas.FillRect(FPaintBox.ClientRect);
    Exit;
  end;

  FPaintBox.Canvas.StretchDraw(FPaintBox.ClientRect, FDisplayBitmap);
end;

procedure TForm6.PaintFrameBitmap(ABitmap: TBitmap; AFrameSize: Integer);
var
  DestRect: TRect;
begin
  if not Assigned(ABitmap) or (ABitmap.Width <= 0) or (ABitmap.Height <= 0) then Exit;
  if not Assigned(FPaintBox) then Exit;

  // 1) FDisplayBitmap'i güncelle (OnPaint fallback)
  if not Assigned(FDisplayBitmap) then
  begin
    FDisplayBitmap             := TBitmap.Create;
    FDisplayBitmap.PixelFormat := pf24bit;
  end;
  if (FDisplayBitmap.Width  <> ABitmap.Width) or
     (FDisplayBitmap.Height <> ABitmap.Height) then
    FDisplayBitmap.SetSize(ABitmap.Width, ABitmap.Height);
  FDisplayBitmap.Canvas.Draw(0, 0, ABitmap);

  // 2) Doğrudan FPaintBox canvas'ına çiz — Invalidate/Erase döngüsü YOK
  DestRect := FPaintBox.ClientRect;
  FPaintBox.Canvas.StretchDraw(DestRect, ABitmap);

  FLastFrameSize := AFrameSize;

  if (GetTickCount64 - FLastStatusTick) >= 500 then
  begin
    FLastStatusTick := GetTickCount64;
    UpdateStatusBar;
  end;
end;

{ -------------------------------------------------------------------------
  Decode — JPEG (legacy) or H.264
  ------------------------------------------------------------------------- }

function TForm6.DecodeBase64Image(const AText: string; out ABytes: TBytes): Boolean;
var
  CleanText: string;
  CommaPos : Integer;
begin
  Result := False;
  SetLength(ABytes, 0);
  CleanText := Trim(AText);
  if CleanText = '' then Exit;

  if StartsText('data:', CleanText) then
  begin
    CommaPos := Pos(',', CleanText);
    if CommaPos > 0 then
      Delete(CleanText, 1, CommaPos);
  end;

  try
    ABytes := TNetEncoding.Base64.DecodeStringToBytes(CleanText);
    Result := Length(ABytes) > 0;
  except
    Result := False;
  end;
end;

function TForm6.DecodeFrameToBitmap(const ABytes: TBytes;
                                     AFormat: Integer;
                                     out ABitmap: TBitmap): Boolean;
var
  Stream   : TMemoryStream;
  JpegImage: TJPEGImage;
  Picture  : TPicture;
  Graphic  : TGraphic;
  DecBmp   : TBitmap;
begin
  Result  := False;
  ABitmap := nil;
  if Length(ABytes) = 0 then Exit;

  { --- H.264 path --- }
  if AFormat = MONITOR_FRAME_FORMAT_H264 then
  begin
    if not Assigned(FH264Decoder) or not FH264Decoder.FInitialized then Exit;
    DecBmp := FH264Decoder.DecodeFrame(ABytes,
                                        FH264Decoder.FWidth,
                                        FH264Decoder.FHeight);
    if Assigned(DecBmp) then
    begin
      ABitmap := DecBmp;
      Result  := True;
    end;
    Exit;
  end;

  { --- H.265 path --- }
  if AFormat = MONITOR_FRAME_FORMAT_H265 then
  begin
    if not Assigned(FH265Decoder) or not FH265Decoder.FInitialized then Exit;
    DecBmp := FH265Decoder.DecodeFrame(ABytes,
                                        FH265Decoder.FWidth,
                                        FH265Decoder.FHeight);
    if Assigned(DecBmp) then
    begin
      ABitmap := DecBmp;
      Result  := True;
    end;
    Exit;
  end;

  { --- JPEG / legacy path --- }
  Stream    := TMemoryStream.Create;
  JpegImage := TJPEGImage.Create;
  Picture   := nil;
  try
    Stream.WriteBuffer(ABytes[0], Length(ABytes));
    Stream.Position := 0;
    try
      JpegImage.LoadFromStream(Stream);
      Graphic := JpegImage;
    except
      Stream.Position := 0;
      Picture := TPicture.Create;
      Picture.LoadFromStream(Stream);
      Graphic := Picture.Graphic;
    end;

    if not Assigned(Graphic) or (Graphic.Width <= 0) or (Graphic.Height <= 0) then
      Exit;

    ABitmap             := TBitmap.Create;
    ABitmap.PixelFormat := pf24bit;
    ABitmap.SetSize(Graphic.Width, Graphic.Height);
    ABitmap.Canvas.Lock;
    try
      ABitmap.Canvas.Draw(0, 0, Graphic);
    finally
      ABitmap.Canvas.Unlock;
    end;
    Result := True;
  finally
    if not Result then FreeAndNil(ABitmap);
    Picture.Free;
    JpegImage.Free;
    Stream.Free;
  end;
end;

{ -------------------------------------------------------------------------
  Background decode worker
  ------------------------------------------------------------------------- }

procedure TForm6.StartFrameWorker;
begin
  if Assigned(FDecodeThread) then Exit;

  if not Assigned(FFrameLock) then
    FFrameLock := TCriticalSection.Create;
  if not Assigned(FDecodeEvent) then
    FDecodeEvent := TEvent.Create(nil, True, False, '');

  FDecodeStopping := False;
  FDecodeThread   := TThread.CreateAnonymousThread(
    procedure
    begin
      DecodeFrameWorker;
    end);
  FDecodeThread.FreeOnTerminate := False;
  FDecodeThread.Start;
end;

procedure TForm6.StopFrameWorker;
begin
  FDecodeStopping := True;
  if Assigned(FDecodeEvent) then
    FDecodeEvent.SetEvent;

  if Assigned(FDecodeThread) then
  begin
    FDecodeThread.WaitFor;
    FreeAndNil(FDecodeThread);
  end;
end;

function TForm6.TakePendingFrame(out AText: string;
                                  out ABytes: TBytes;
                                  out AFormat: Integer): Boolean;
var
  W, H: Integer;
begin
  Result  := False;
  AText   := '';
  SetLength(ABytes, 0);
  AFormat := MONITOR_FRAME_FORMAT_JPEG;
  W := 0; H := 0;

  if not Assigned(FFrameLock) then Exit;

  FFrameLock.Enter;
  try
    if Length(FPendingFrameBytes) > 0 then
    begin
      ABytes  := FPendingFrameBytes;
      AFormat := FPendingFrameFormat;
      W       := FPendingFrameWidth;
      H       := FPendingFrameHeight;
      SetLength(FPendingFrameBytes, 0);
      FPendingFrame := '';
      Result := True;
    end
    else if FPendingFrame <> '' then
    begin
      AText         := FPendingFrame;
      AFormat       := MONITOR_FRAME_FORMAT_JPEG;
      FPendingFrame := '';
      Result        := True;
    end;

    if (FPendingFrame = '') and (Length(FPendingFrameBytes) = 0) and
       Assigned(FDecodeEvent) then
      FDecodeEvent.ResetEvent;
  finally
    FFrameLock.Leave;
  end;

  { Update the H.264/H.265 decoder hint — done outside the lock }
  if Result and (AFormat = MONITOR_FRAME_FORMAT_H264) and
     Assigned(FH264Decoder) and (W > 0) and (H > 0) then
    FH264Decoder.TryInit(W, H);

  if Result and (AFormat = MONITOR_FRAME_FORMAT_H265) and
     Assigned(FH265Decoder) and (W > 0) and (H > 0) then
    FH265Decoder.TryInit(W, H);
end;

function TForm6.TakeDecodedFrame(out ABitmap: TBitmap; out AFrameSize: Integer): Boolean;
begin
  Result     := False;
  ABitmap    := nil;
  AFrameSize := 0;

  if not Assigned(FFrameLock) then Exit;

  FFrameLock.Enter;
  try
    if Assigned(FDecodedBitmap) then
    begin
      ABitmap           := FDecodedBitmap;
      AFrameSize        := FDecodedFrameSize;
      FDecodedBitmap    := nil;
      FDecodedFrameSize := 0;
      Result            := True;
    end;
  finally
    FFrameLock.Leave;
  end;
end;

procedure TForm6.DecodeFrameWorker;
var
  Text     : string;
  Bytes    : TBytes;
  Fmt      : Integer;
  Decoded  : TBitmap;
  FrameSize: Integer;
begin
  while not FDecodeStopping do
  begin
    if Assigned(FDecodeEvent) then
      FDecodeEvent.WaitFor(100);

    while (not FDecodeStopping) and TakePendingFrame(Text, Bytes, Fmt) do
    begin
      { Base64 text → raw bytes (legacy JSON path) }
      if (Length(Bytes) = 0) and (Text <> '') then
      begin
        DecodeBase64Image(Text, Bytes);
        Fmt := MONITOR_FRAME_FORMAT_JPEG;
      end;

      FrameSize := Length(Bytes);
      Decoded   := nil;
      try
        if DecodeFrameToBitmap(Bytes, Fmt, Decoded) then
        begin
          FFrameLock.Enter;
          try
            FreeAndNil(FDecodedBitmap);
            FDecodedBitmap    := Decoded;
            FDecodedFrameSize := FrameSize;
            Decoded           := nil;
          finally
            FFrameLock.Leave;
          end;
        end;
      except
        FreeAndNil(Decoded);
      end;
    end;
  end;
end;

{ -------------------------------------------------------------------------
  Monitor list / Status / JSON handling
  ------------------------------------------------------------------------- }

procedure TForm6.UpdateMonitorList(JSONObj: TJSONObject);
var
  MonitorsVal: TJSONValue;
  MonitorsArr: TJSONArray;
  MonitorVal : TJSONValue;
  MonitorObj : TJSONObject;
  Name       : string;
  i          : Integer;
begin
  MonitorsVal := JSONObj.Values['monitors'];
  if not (MonitorsVal is TJSONArray) then Exit;

  MonitorsArr := TJSONArray(MonitorsVal);
  if MonitorsArr.Count = 0 then Exit;

  ComboBox2.Items.BeginUpdate;
  try
    ComboBox2.Items.Clear;
    for i := 0 to MonitorsArr.Count - 1 do
    begin
      MonitorVal := MonitorsArr.Items[i];
      Name       := '';

      if MonitorVal is TJSONObject then
      begin
        MonitorObj := TJSONObject(MonitorVal);
        Name       := JSONValueText(MonitorObj, 'name');
        if Name = '' then
          Name := 'Monitor ' + IntToStr(i + 1);
      end
      else
        Name := MonitorVal.Value;

      ComboBox2.Items.Add(Name);
    end;

    if ComboBox2.ItemIndex < 0 then
      ComboBox2.ItemIndex := 0;
  finally
    ComboBox2.Items.EndUpdate;
  end;
end;

procedure TForm6.UpdateStatusBar;
var
  CaptureText: string;
  SizeText   : string;
  CodecText  : string;
begin
  if FCapturing then
    CaptureText := 'Capturing [On]'
  else
    CaptureText := 'Capturing [Off]';

  CodecText := 'Codec [' + UpperCase(FCurrentCodec) + ']';

  if FLastFrameSize > 0 then
    SizeText := 'Frame [' + FormatFloat('0.0 KB', FLastFrameSize / 1024) + ']'
  else
    SizeText := 'Frame []';

  if StatusBar1.Panels.Count >= 2 then
  begin
    StatusBar1.Panels[0].Text := CaptureText + '  ' + CodecText;
    StatusBar1.Panels[1].Text := SizeText;
  end
  else
    StatusBar1.SimpleText := CaptureText + '  ' + CodecText + '  ' + SizeText;
end;

procedure TForm6.UpdateButtonCaption;
begin
  if FCapturing then
    Button1.Caption := 'Stop Capture'
  else
    Button1.Caption := 'Start Capture';
end;

procedure TForm6.HandleMonitoringJSON(JSONObj: TJSONObject);
var
  StatusText: string;
  ImageText : string;
  ErrorText : string;
  CodecText : string;
begin
  if JSONObj = nil then Exit;

  UpdateMonitorList(JSONObj);

  StatusText := JSONValueText(JSONObj, 'status');
  if SameText(StatusText, 'started') then
    FCapturing := True
  else if SameText(StatusText, 'stopped') then
    FCapturing := False;

  { Track codec reported by the client }
  CodecText := JSONValueText(JSONObj, 'codec');
  if CodecText <> '' then
    FCurrentCodec := LowerCase(CodecText);

  ErrorText := JSONValueText(JSONObj, 'error');
  if ErrorText <> '' then
  begin
    FCapturing := False;
    UpdateButtonCaption;
    if StatusBar1.Panels.Count >= 2 then
    begin
      StatusBar1.Panels[0].Text := 'Capturing [Error]';
      StatusBar1.Panels[1].Text := ErrorText;
    end
    else
      StatusBar1.SimpleText := 'Capturing [Error]  ' + ErrorText;
    Exit;
  end;

  ImageText := JSONValueText(JSONObj, 'image');
  if ImageText = '' then ImageText := JSONValueText(JSONObj, 'frame');
  if ImageText = '' then ImageText := JSONValueText(JSONObj, 'data');

  if ImageText <> '' then
    QueueFrame(ImageText);

  if (StatusText <> '') or (ImageText = '') then
    UpdateButtonCaption;
  if ImageText = '' then
    UpdateStatusBar;
end;

{ -------------------------------------------------------------------------
  Mouse / Keyboard input forwarding
  ------------------------------------------------------------------------- }

procedure TForm6.FPaintBoxMouseDown(Sender: TObject; Button: TMouseButton;
  Shift: TShiftState; X, Y: Integer);
var
  JSONObj: TJSONObject;
begin
  if not CheckBox2.Checked or not Assigned(FOnSendJSON) or not Assigned(FLine) then Exit;

  JSONObj := TJSONObject.Create;
  try
    JSONObj.AddPair('action', 'mouseevent');
    JSONObj.AddPair('event',  'down');
    JSONObj.AddPair('button', TJSONNumber.Create(Ord(Button)));
    JSONObj.AddPair('x',      TJSONNumber.Create(Round(X * 65535 / Max(1, FPaintBox.Width))));
    JSONObj.AddPair('y',      TJSONNumber.Create(Round(Y * 65535 / Max(1, FPaintBox.Height))));
    FOnSendJSON(FLine, JSONObj);
  finally
    JSONObj.Free;
  end;
end;

procedure TForm6.FPaintBoxMouseMove(Sender: TObject; Shift: TShiftState; X, Y: Integer);
var
  JSONObj: TJSONObject;
  NowTick: UInt64;
begin
  if not CheckBox2.Checked or not Assigned(FOnSendJSON) or not Assigned(FLine) then Exit;

  NowTick := GetTickCount64;
  if (NowTick - FLastMouseMoveTick) < 30 then Exit;
  FLastMouseMoveTick := NowTick;

  JSONObj := TJSONObject.Create;
  try
    JSONObj.AddPair('action', 'mouseevent');
    JSONObj.AddPair('event',  'move');
    JSONObj.AddPair('x',      TJSONNumber.Create(Round(X * 65535 / Max(1, FPaintBox.Width))));
    JSONObj.AddPair('y',      TJSONNumber.Create(Round(Y * 65535 / Max(1, FPaintBox.Height))));
    FOnSendJSON(FLine, JSONObj);
  finally
    JSONObj.Free;
  end;
end;

procedure TForm6.FPaintBoxMouseUp(Sender: TObject; Button: TMouseButton;
  Shift: TShiftState; X, Y: Integer);
var
  JSONObj: TJSONObject;
begin
  if not CheckBox2.Checked or not Assigned(FOnSendJSON) or not Assigned(FLine) then Exit;

  JSONObj := TJSONObject.Create;
  try
    JSONObj.AddPair('action', 'mouseevent');
    JSONObj.AddPair('event',  'up');
    JSONObj.AddPair('button', TJSONNumber.Create(Ord(Button)));
    JSONObj.AddPair('x',      TJSONNumber.Create(Round(X * 65535 / Max(1, FPaintBox.Width))));
    JSONObj.AddPair('y',      TJSONNumber.Create(Round(Y * 65535 / Max(1, FPaintBox.Height))));
    FOnSendJSON(FLine, JSONObj);
  finally
    JSONObj.Free;
  end;
end;

procedure TForm6.FormKeyDown(Sender: TObject; var Key: Word; Shift: TShiftState);
var
  JSONObj: TJSONObject;
begin
  if not CheckBox1.Checked or not Assigned(FOnSendJSON) or not Assigned(FLine) then Exit;

  JSONObj := TJSONObject.Create;
  try
    JSONObj.AddPair('action', 'keyevent');
    JSONObj.AddPair('event',  'down');
    JSONObj.AddPair('key',    TJSONNumber.Create(Key));
    FOnSendJSON(FLine, JSONObj);
  finally
    JSONObj.Free;
  end;
end;

procedure TForm6.FormKeyUp(Sender: TObject; var Key: Word; Shift: TShiftState);
var
  JSONObj: TJSONObject;
begin
  if not CheckBox1.Checked or not Assigned(FOnSendJSON) or not Assigned(FLine) then Exit;

  JSONObj := TJSONObject.Create;
  try
    JSONObj.AddPair('action', 'keyevent');
    JSONObj.AddPair('event',  'up');
    JSONObj.AddPair('key',    TJSONNumber.Create(Key));
    FOnSendJSON(FLine, JSONObj);
  finally
    JSONObj.Free;
  end;
end;

{ -------------------------------------------------------------------------
  Unit finalization — MF cleanup
  ------------------------------------------------------------------------- }
initialization

finalization
  if GMFStarted and Assigned(GMFShutdown) then
  begin
    GMFShutdown;
    GMFStarted := False;
  end;
  if GHMFPlat <> 0 then FreeLibrary(GHMFPlat);
  if GHMFLib  <> 0 then FreeLibrary(GHMFLib);
  FreeAndNil(GMFLock);

end.

