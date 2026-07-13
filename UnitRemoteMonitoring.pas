unit UnitRemoteMonitoring;

interface

uses
  Winapi.Windows, Winapi.Messages, Winapi.ActiveX,
  System.SysUtils, System.Classes, System.JSON, System.Math,
  System.NetEncoding, System.StrUtils, System.SyncObjs,
  Vcl.Graphics, Vcl.Controls, Vcl.Forms, Vcl.Dialogs, Vcl.StdCtrls,
  Vcl.ComCtrls, Vcl.ExtCtrls, Vcl.Imaging.jpeg, Vcl.Imaging.pngimage,
  ncLines;

type
  TMonitoringSendJSONEvent   = procedure(aLine: TncLine; JSONObj: TJSONObject) of object;
  TMonitoringFormClosedEvent = procedure(aLine: TncLine) of object;

  // Flicker'siz PaintBox: WM_ERASEBKGND'yi engeller
  TNoFlickerPaintBox = class(TPaintBox)
  protected
    procedure WMEraseBkgnd(var Msg: TWMEraseBkgnd); message WM_ERASEBKGND;
  end;

const
  MONITOR_FRAME_FORMAT_H264 = 2;

  IID_IMFTransform: TGUID = '{bf94c121-5b05-4e6f-8000-ba0092c7e175}';
  IID_IMFMediaType: TGUID = '{3666a324-4e09-43a1-bc39-a0a353664d0d}';
  IID_IMFMediaBuffer: TGUID = '{04569536-a71f-4dfd-8913-aa2d1180c9ee}';
  IID_IMFSample: TGUID = '{c40a73d0-70b9-4d7c-a10f-7139228c2225}';
  IID_IMFAttributes: TGUID = '{2cd2d121-98a5-4ae1-831d-b488ef63ca1f}';

  CLSID_CMSH264DecoderMFT: TGUID = '{62f0ad17-b16f-451f-ab3b-39906bc41907}';

  MF_MT_MAJOR_TYPE: TGUID = '{48eba18e-f8c9-4687-bf11-0a74c9f96a8f}';
  MF_MT_SUBTYPE: TGUID = '{f7e34c9a-425f-4e15-aa3c-5a224d3fd0bc}';
  MF_MT_FRAME_SIZE: TGUID = '{16617f81-6e35-4ed4-a13d-d22e1851547a}';
  MF_MT_FRAME_RATE: TGUID = '{c459a22c-ec16-4b9e-816a-0d5914bf5d1d}';
  MF_MT_INTERLACE_MODE: TGUID = '{e2724d27-4344-4e55-a0e1-089d3e44973e}';
  MF_MT_PIXEL_ASPECT_RATIO: TGUID = '{c6376a1e-8d0a-4027-be45-6d9a0ad39d2a}';

  MFMediaType_Video: TGUID = '{73646976-0000-0010-8000-00aa00389b71}';
  MFVideoFormat_H264: TGUID = '{34363248-0000-0010-8000-00aa00389b71}';
  MFVideoFormat_NV12: TGUID = '{3231564e-0000-0010-8000-00aa00389b71}';

  MF_VERSION = $0002;
  MFT_MESSAGE_COMMAND_FLUSH = $00000001;
  MFT_MESSAGE_NOTIFY_BEGIN_STREAMING = $10000000;
  MFT_MESSAGE_NOTIFY_START_OF_STREAM = $10000003;
  MF_E_TRANSFORM_STREAM_CHANGE = HRESULT($C00D6D73);
  MF_E_TRANSFORM_NEED_MORE_INPUT = HRESULT($C00D6D72);

type
  IMFMediaBuffer = interface;
  IMFSample = interface;
  IMFMediaType = interface;
  IMFAttributes = interface;
  IMFTransform = interface;

  IMFMediaBuffer = interface(IUnknown)
    ['{04569536-a71f-4dfd-8913-aa2d1180c9ee}']
    function Lock(out ppbBuffer: PByte; pdwMaxLength: PCardinal; pdwCurrentLength: PCardinal): HResult; stdcall;
    function Unlock: HResult; stdcall;
    function GetCurrentLength(out pcbCurrentLength: Cardinal): HResult; stdcall;
    function SetCurrentLength(cbCurrentLength: Cardinal): HResult; stdcall;
    function GetMaxLength(out pcbMaxLength: Cardinal): HResult; stdcall;
  end;

  IMFAttributes = interface(IUnknown)
    ['{2cd2d121-98a5-4ae1-831d-b488ef63ca1f}']
    function GetItem(const guidKey: TGUID; pValue: Pointer): HResult; stdcall;
    function GetItemType(const guidKey: TGUID; out pType: Integer): HResult; stdcall;
    function IsKeyPresent(const guidKey: TGUID; out pfPresent: BOOL): HResult; stdcall;
    function GetUINT32(const guidKey: TGUID; out punValue: Cardinal): HResult; stdcall;
    function GetUINT64(const guidKey: TGUID; out punValue: UInt64): HResult; stdcall;
    function GetDouble(const guidKey: TGUID; out pfValue: Double): HResult; stdcall;
    function GetGUID(const guidKey: TGUID; out pguidValue: TGUID): HResult; stdcall;
    function GetStringLength(const guidKey: TGUID; out pcchLength: Cardinal): HResult; stdcall;
    function GetString(const guidKey: TGUID; pwszValue: PWideChar; cchMaxLength: Cardinal; pcchLength: PCardinal): HResult; stdcall;
    function GetAllocatedString(const guidKey: TGUID; out ppwszValue: PWideChar; out pcchLength: Cardinal): HResult; stdcall;
    function GetBlobSize(const guidKey: TGUID; out pcbBufSize: Cardinal): HResult; stdcall;
    function GetBlob(const guidKey: TGUID; pBuf: PByte; cbBufSize: Cardinal; pcbBufSize: PCardinal): HResult; stdcall;
    function GetAllocatedBlob(const guidKey: TGUID; out ppBuf: PByte; out pcbSize: Cardinal): HResult; stdcall;
    function GetUnknown(const guidKey: TGUID; const riid: TGUID; out ppv: Pointer): HResult; stdcall;
    function SetItem(const guidKey: TGUID; const Value: Pointer): HResult; stdcall;
    function DeleteItem(const guidKey: TGUID): HResult; stdcall;
    function DeleteAllItems: HResult; stdcall;
    function SetUINT32(const guidKey: TGUID; unValue: Cardinal): HResult; stdcall;
    function SetUINT64(const guidKey: TGUID; unValue: UInt64): HResult; stdcall;
    function SetDouble(const guidKey: TGUID; fValue: Double): HResult; stdcall;
    function SetGUID(const guidKey: TGUID; const guidValue: TGUID): HResult; stdcall;
    function SetString(const guidKey: TGUID; pwszValue: PWideChar): HResult; stdcall;
    function SetBlob(const guidKey: TGUID; const pBuf: PByte; cbBufSize: Cardinal): HResult; stdcall;
    function SetUnknown(const guidKey: TGUID; pUnknown: IUnknown): HResult; stdcall;
    function LockStore: HResult; stdcall;
    function UnlockStore: HResult; stdcall;
    function GetCount(out pcItems: Cardinal): HResult; stdcall;
    function GetItemByIndex(unIndex: Cardinal; out pguidKey: TGUID; pValue: Pointer): HResult; stdcall;
    function CopyAllItems(pDest: IMFAttributes): HResult; stdcall;
  end;

  IMFSample = interface(IMFAttributes)
    ['{c40a73d0-70b9-4d7c-a10f-7139228c2225}']
    function GetSampleFlags(out pdwFlags: Cardinal): HResult; stdcall;
    function SetSampleFlags(dwFlags: Cardinal): HResult; stdcall;
    function GetSampleTime(out phnsSampleTime: Int64): HResult; stdcall;
    function SetSampleTime(hnsSampleTime: Int64): HResult; stdcall;
    function GetSampleDuration(out phnsSampleDuration: Int64): HResult; stdcall;
    function SetSampleDuration(hnsSampleDuration: Int64): HResult; stdcall;
    function GetBufferCount(out pdwBufferCount: Cardinal): HResult; stdcall;
    function GetBufferByIndex(dwIndex: Cardinal; out ppBuffer: IMFMediaBuffer): HResult; stdcall;
    function ConvertToContiguousBuffer(out ppBuffer: IMFMediaBuffer): HResult; stdcall;
    function AddBuffer(pBuffer: IMFMediaBuffer): HResult; stdcall;
    function RemoveBufferByIndex(dwIndex: Cardinal): HResult; stdcall;
    function RemoveAllBuffers: HResult; stdcall;
    function GetTotalLength(out pcbTotalLength: Cardinal): HResult; stdcall;
    function CopyAllSamples(pDest: IMFSample): HResult; stdcall;
  end;

  IMFMediaType = interface(IMFAttributes)
    ['{3666a324-4e09-43a1-bc39-a0a353664d0d}']
    function GetMajorType(out pguidMajorType: TGUID): HResult; stdcall;
    function IsCompressedFormat(out pfCompressed: BOOL): HResult; stdcall;
    function CheckIntermediateType(const guidSubtype: TGUID; out pfSupported: BOOL): HResult; stdcall;
    function GetRepresentation(guidRepresentation: TGUID; out ppRepresentation: Pointer): HResult; stdcall;
    function FreeRepresentation(guidRepresentation: TGUID; pRepresentation: Pointer): HResult; stdcall;
  end;

  TTransformInputStreamInfo = record
    hnsMaxLatency: Int64;
    dwFlags: Cardinal;
    cbSize: Cardinal;
    cbMaxLookahead: Cardinal;
    cbAlignment: Cardinal;
  end;

  TTransformOutputStreamInfo = record
    dwFlags: Cardinal;
    cbSize: Cardinal;
    cbAlignment: Cardinal;
  end;

  TOutputDataBuffer = record
    dwStreamID: Cardinal;
    pSample: IMFSample;
    dwStatus: Cardinal;
    pEvents: IUnknown;
  end;
  POutputDataBuffer = ^TOutputDataBuffer;

  IMFTransform = interface(IUnknown)
    ['{bf94c121-5b05-4e6f-8000-ba0092c7e175}']
    function GetStreamLimits(pdwInputMinimum, pdwInputMaximum, pdwOutputMinimum, pdwOutputMaximum: PCardinal): HResult; stdcall;
    function GetStreamCount(pdwInputStreams, pdwOutputStreams: PCardinal): HResult; stdcall;
    function GetStreamIDs(dwInputIDArraySize: Cardinal; pdwInputIDs: PCardinal; dwOutputIDArraySize: Cardinal; pdwOutputIDs: PCardinal): HResult; stdcall;
    function GetInputStreamInfo(dwInputStreamID: Cardinal; out pStreamInfo: TTransformInputStreamInfo): HResult; stdcall;
    function GetOutputStreamInfo(dwOutputStreamID: Cardinal; out pStreamInfo: TTransformOutputStreamInfo): HResult; stdcall;
    function GetAttributes(out ppAttributes: IMFAttributes): HResult; stdcall;
    function GetInputAvailableType(dwInputStreamID: Cardinal; dwTypeIndex: Cardinal; out ppType: IMFMediaType): HResult; stdcall;
    function GetOutputAvailableType(dwOutputStreamID: Cardinal; dwTypeIndex: Cardinal; out ppType: IMFMediaType): HResult; stdcall;
    function SetInputType(dwInputStreamID: Cardinal; pType: IMFMediaType; dwFlags: Cardinal): HResult; stdcall;
    function SetOutputType(dwOutputStreamID: Cardinal; pType: IMFMediaType; dwFlags: Cardinal): HResult; stdcall;
    function GetInputCurrentType(dwInputStreamID: Cardinal; out ppType: IMFMediaType): HResult; stdcall;
    function GetOutputCurrentType(dwOutputStreamID: Cardinal; out ppType: IMFMediaType): HResult; stdcall;
    function GetInputStatus(dwInputStreamID: Cardinal; out pdwFlags: Cardinal): HResult; stdcall;
    function GetOutputStatus(out pdwFlags: Cardinal): HResult; stdcall;
    function SetOutputBounds(hnsLowerBound: Int64; hnsUpperBound: Int64): HResult; stdcall;
    function ProcessEvent(dwInputStreamID: Cardinal; pEvent: IUnknown): HResult; stdcall;
    function ProcessMessage(eMessage: Cardinal; ulParam: ULONG_PTR): HResult; stdcall;
    function ProcessInput(dwInputStreamID: Cardinal; pSample: IMFSample; dwFlags: Cardinal): HResult; stdcall;
    function ProcessOutput(dwFlags: Cardinal; cOutputBufferCount: Cardinal; pOutputSamples: POutputDataBuffer; out pdwStatus: Cardinal): HResult; stdcall;
  end;

  TMFVideoDecoder = class
  private
    FDecoder: IMFTransform;
    FWidth: Integer;
    FHeight: Integer;
    FInitialized: Boolean;
    procedure ConfigureDecoder;
  public
    constructor Create(AWidth, AHeight: Integer);
    destructor Destroy; override;
    function DecodeFrame(const AInputBytes: TBytes; out ARawBytes: TBytes): Boolean;
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
    ComboBox3: TComboBox;          // Designer added ComboBox3
    PaintBox1: TPaintBox;          // DFM'de kalır ama gizlenecek
    procedure Button1Click(Sender: TObject);
    procedure ComboBox1Change(Sender: TObject);
    procedure ComboBox2Change(Sender: TObject);
    procedure ComboBox3Change(Sender: TObject);
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
    FPendingFormat    : Integer;
    FPendingWidth     : Integer;
    FPendingHeight    : Integer;
    FDecoder          : TMFVideoDecoder;
    FCurrentDecFormat : Integer;
    FCurrentDecWidth  : Integer;
    FCurrentDecHeight : Integer;
    FFrameLock        : TCriticalSection;
    FDecodeEvent      : TEvent;
    FDecodeThread     : TThread;
    FDecodeStopping   : Boolean;
    FDecodedRawBytes  : TBytes;
    FDecodedRawWidth  : Integer;
    FDecodedRawHeight : Integer;
    FDecodedFrameSize : Integer;
    FDisplayBitmap    : TBitmap;         // Repaint fallback için
    FPaintBox         : TNoFlickerPaintBox; // Gerçek görüntü alanı
    FLastMouseMoveTick: UInt64;

    procedure FillDefaultOptions;
    procedure SendMonitoringCommand(const AAction: string);
    function  SelectedMonitorIndex: Integer;
    function  SelectedScalePercent: Integer;
    function  JSONValueText(JSONObj: TJSONObject; const AName: string): string;
    function  DecodeBase64Image(const AText: string; out ABytes: TBytes): Boolean;

    function  DecodeFrameToRawBGR32(const ABytes: TBytes; out ARawBytes: TBytes; out AWidth, AHeight: Integer): Boolean;
    procedure QueueFrame(const AText: string);
    procedure FrameTimerTimer(Sender: TObject);
    procedure PaintBoxPaint(Sender: TObject);
    procedure PaintFrameBitmap(ABitmap: TBitmap; AFrameSize: Integer);
    procedure StartFrameWorker;
    procedure StopFrameWorker;
    procedure DecodeFrameWorker;
    function  TakePendingFrame(out AText: string; out ABytes: TBytes; out AFormat, AWidth, AHeight: Integer): Boolean;
    function  TakeDecodedFrame(out ARawBytes: TBytes; out AWidth, AHeight, AFrameSize: Integer): Boolean;
    procedure ComboBox3Change(Sender: TObject);
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
    procedure QueueFrameBytes(const ABytes: TBytes; AFormat, AWidth, AHeight: Integer);
    procedure HandleMonitoringJSON(JSONObj: TJSONObject);
  end;

var
  Form6: TForm6;

implementation

{$R *.dfm}

function FAILED(hr: HResult): Boolean; inline;
begin
  Result := hr < 0;
end;

function SUCCEEDED(hr: HResult): Boolean; inline;
begin
  Result := hr >= 0;
end;

var
  MFStartup: function(Version: Cardinal; dwFlags: Cardinal = 0): HResult; stdcall = nil;
  MFShutdown: function: HResult; stdcall = nil;
  MFCreateMediaType: function(out ppMFType: IMFMediaType): HResult; stdcall = nil;
  MFCreateMemoryBuffer: function(cbMaxLength: Cardinal; out ppBuffer: IMFMediaBuffer): HResult; stdcall = nil;
  MFCreateSample: function(out ppSample: IMFSample): HResult; stdcall = nil;
  MFTEnumEx: function(guidCategory: TGUID; Flags: Cardinal; pInputType: Pointer; pOutputType: Pointer; out pppMFTActivate: Pointer; out pcMFTActivate: Cardinal): HResult; stdcall = nil;

procedure LoadMF;
var
  H: THandle;
begin
  if Assigned(MFStartup) then Exit;
  H := LoadLibrary('mfplat.dll');
  if H <> 0 then
  begin
    @MFStartup := GetProcAddress(H, 'MFStartup');
    @MFShutdown := GetProcAddress(H, 'MFShutdown');
    @MFCreateMediaType := GetProcAddress(H, 'MFCreateMediaType');
    @MFCreateMemoryBuffer := GetProcAddress(H, 'MFCreateMemoryBuffer');
    @MFCreateSample := GetProcAddress(H, 'MFCreateSample');
    @MFTEnumEx := GetProcAddress(H, 'MFTEnumEx');
  end;
end;

procedure NV12_To_BGR32(const ANV12: PByte; AWidth, AHeight: Integer; out ARawBytes: TBytes);
var
  YPlane, UVPlane: PByte;
  y, x: Integer;
  YVal, UVal, VVal: Integer;
  R, G, B: Integer;
  C, D, E: Integer;
  PixelIdx: Integer;
begin
  SetLength(ARawBytes, AWidth * AHeight * 4);

  YPlane := ANV12;
  UVPlane := ANV12 + (AWidth * AHeight);

  for y := 0 to AHeight - 1 do
  begin
    for x := 0 to AWidth - 1 do
    begin
      YVal := YPlane[y * AWidth + x];
      UVal := UVPlane[(y div 2) * AWidth + ((x div 2) * 2)];
      VVal := UVPlane[(y div 2) * AWidth + ((x div 2) * 2) + 1];

      C := YVal - 16;
      D := UVal - 128;
      E := VVal - 128;

      R := (298 * C + 409 * E + 128) div 256;
      G := (298 * C - 100 * D - 208 * E + 128) div 256;
      B := (298 * C + 516 * D + 128) div 256;

      if R < 0 then R := 0 else if R > 255 then R := 255;
      if G < 0 then G := 0 else if G > 255 then G := 255;
      if B < 0 then B := 0 else if B > 255 then B := 255;

      PixelIdx := (y * AWidth + x) * 4;
      ARawBytes[PixelIdx] := Byte(B);
      ARawBytes[PixelIdx + 1] := Byte(G);
      ARawBytes[PixelIdx + 2] := Byte(R);
      ARawBytes[PixelIdx + 3] := 0;
    end;
  end;
end;

{ TMFVideoDecoder }

constructor TMFVideoDecoder.Create(AWidth, AHeight: Integer);
begin
  inherited Create;
  FWidth := AWidth;
  FHeight := AHeight;
  ConfigureDecoder;
end;

destructor TMFVideoDecoder.Destroy;
begin
  FDecoder := nil;
  if Assigned(MFShutdown) then
    MFShutdown();
  inherited;
end;

procedure TMFVideoDecoder.ConfigureDecoder;
var
  hr: HResult;
  InputType, OutputType: IMFMediaType;
begin
  LoadMF;
  if not Assigned(MFStartup) then Exit;

  hr := MFStartup(MF_VERSION, 0);
  if FAILED(hr) then Exit;

  hr := CoCreateInstance(CLSID_CMSH264DecoderMFT, nil, CLSCTX_INPROC_SERVER, IID_IMFTransform, FDecoder);
  if FAILED(hr) or (FDecoder = nil) then Exit;

  hr := MFCreateMediaType(InputType);
  if SUCCEEDED(hr) then
  begin
    InputType.SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    InputType.SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    InputType.SetUINT64(MF_MT_FRAME_SIZE, (UInt64(FWidth) shl 32) or Cardinal(FHeight));
    FDecoder.SetInputType(0, InputType, 0);
  end;

  hr := MFCreateMediaType(OutputType);
  if SUCCEEDED(hr) then
  begin
    OutputType.SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    OutputType.SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    OutputType.SetUINT64(MF_MT_FRAME_SIZE, (UInt64(FWidth) shl 32) or Cardinal(FHeight));
    FDecoder.SetOutputType(0, OutputType, 0);
  end;

  FDecoder.ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
  FDecoder.ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  FDecoder.ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  FInitialized := True;
end;

function TMFVideoDecoder.DecodeFrame(const AInputBytes: TBytes; out ARawBytes: TBytes): Boolean;
var
  hr: HResult;
  InputSample: IMFSample;
  InputBuffer: IMFMediaBuffer;
  P: PByte;
  MaxLen, CurLen: Cardinal;
  StreamInfo: TTransformOutputStreamInfo;
  OutputData: TOutputDataBuffer;
  OutputSample: IMFSample;
  OutputBuffer: IMFMediaBuffer;
  Status: Cardinal;
  OutputType: IMFMediaType;
  NV12Ptr: PByte;
begin
  Result := False;
  SetLength(ARawBytes, 0);
  if not FInitialized or (FDecoder = nil) then Exit;

  hr := MFCreateSample(InputSample);
  if FAILED(hr) then Exit;

  hr := MFCreateMemoryBuffer(Length(AInputBytes), InputBuffer);
  if FAILED(hr) then Exit;

  hr := InputBuffer.Lock(P, @MaxLen, @CurLen);
  if SUCCEEDED(hr) then
  begin
    Move(AInputBytes[0], P^, Length(AInputBytes));
    InputBuffer.Unlock;
    InputBuffer.SetCurrentLength(Length(AInputBytes));
  end;
  InputSample.AddBuffer(InputBuffer);

  hr := FDecoder.ProcessInput(0, InputSample, 0);
  if FAILED(hr) then Exit;

  FDecoder.GetOutputStreamInfo(0, StreamInfo);

  FillChar(OutputData, SizeOf(OutputData), 0);
  OutputData.dwStreamID := 0;

  if (StreamInfo.dwFlags and $00000100) = 0 then
  begin
    hr := MFCreateSample(OutputSample);
    if FAILED(hr) then Exit;
    hr := MFCreateMemoryBuffer(StreamInfo.cbSize, OutputBuffer);
    if FAILED(hr) then Exit;
    OutputSample.AddBuffer(OutputBuffer);
    OutputData.pSample := OutputSample;
  end;

  hr := FDecoder.ProcessOutput(0, 1, @OutputData, Status);
  if hr = MF_E_TRANSFORM_STREAM_CHANGE then
  begin
    FDecoder.GetOutputAvailableType(0, 0, OutputType);
    FDecoder.SetOutputType(0, OutputType, 0);
    hr := FDecoder.ProcessOutput(0, 1, @OutputData, Status);
  end;

  if SUCCEEDED(hr) and (OutputData.pSample <> nil) then
  begin
    hr := OutputData.pSample.GetBufferByIndex(0, OutputBuffer);
    if SUCCEEDED(hr) then
    begin
      hr := OutputBuffer.Lock(NV12Ptr, @MaxLen, @CurLen);
      if SUCCEEDED(hr) then
      begin
        NV12_To_BGR32(NV12Ptr, FWidth, FHeight, ARawBytes);
        OutputBuffer.Unlock;
        Result := True;
      end;
    end;
  end;
end;

{ TNoFlickerPaintBox }

procedure TNoFlickerPaintBox.WMEraseBkgnd(var Msg: TWMEraseBkgnd);
begin
  // Arka plan silmeyi tamamen engelle → siyah flash yok
  Msg.Result := 1;
end;

{ TForm6 }

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
  FreeAndNil(FFrameTimer);
  SetLength(FDecodedRawBytes, 0);
  FreeAndNil(FDisplayBitmap);
  FreeAndNil(FDecodeEvent);
  FreeAndNil(FFrameLock);
  if Assigned(FDecoder) then
    FreeAndNil(FDecoder);
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
  FLine             := aLine;
  FClientID         := AClientID;
  FOnSendJSON       := ASendJSON;
  FOnFormClosed     := AFormClosed;
  FCapturing        := False;
  FLastFrameSize    := 0;
  FLastStatusTick   := 0;
  FPendingFrame     := '';
  SetLength(FPendingFrameBytes, 0);
  FPendingFormat    := 1;
  FPendingWidth     := 0;
  FPendingHeight    := 0;
  FDecoder          := nil;
  FCurrentDecFormat := 0;
  FCurrentDecWidth  := 0;
  FCurrentDecHeight := 0;
  FDecodeStopping   := False;
  FDecodedFrameSize := 0;

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

  if Assigned(ComboBox3) then
  begin
    ComboBox3.OnChange := ComboBox3Change;
    if ComboBox3.Items.Count = 0 then
    begin
      ComboBox3.Items.Add('JPEG');
      ComboBox3.Items.Add('H.264');
    end;
    if ComboBox3.ItemIndex < 0 then
    begin
      ComboBox3.ItemIndex := ComboBox3.Items.IndexOf('H.264');
      if ComboBox3.ItemIndex < 0 then
        ComboBox3.ItemIndex := 1;
    end;
  end;

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
  SelectedFormat: Integer;
begin
  if not Assigned(FLine) or not Assigned(FOnSendJSON) then
    Exit;

  SelectedFormat := 2; // Default to H.264 (2)
  if Assigned(ComboBox3) then
  begin
    if SameText(ComboBox3.Text, 'JPEG') then
      SelectedFormat := 1
    else
      SelectedFormat := 2;
  end;

  JSONObj := TJSONObject.Create;
  try
    JSONObj.AddPair('action',  AAction);
    JSONObj.AddPair('monitor', TJSONNumber.Create(SelectedMonitorIndex));
    JSONObj.AddPair('scale',   TJSONNumber.Create(SelectedScalePercent));
    JSONObj.AddPair('format',  TJSONNumber.Create(SelectedFormat));
    FOnSendJSON(FLine, JSONObj);
  finally
    JSONObj.Free;
  end;
end;

procedure TForm6.ComboBox3Change(Sender: TObject);
begin
  if FCapturing then
    SendMonitoringCommand('monitorstart');
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
      SetLength(FDecodedRawBytes, 0);
      FDecodedFrameSize := 0;
      if Assigned(FDecodeEvent) then
        FDecodeEvent.ResetEvent;
    finally
      FFrameLock.Leave;
    end;
  end;
  if Assigned(FFrameTimer) then
    FFrameTimer.Enabled := False;
  if Assigned(FDecoder) then
    FreeAndNil(FDecoder);
  FCurrentDecFormat := 0;
  FCurrentDecWidth  := 0;
  FCurrentDecHeight := 0;
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
  if JSONObj = nil then
    Exit;

  Val := JSONObj.Values[AName];
  if Assigned(Val) then
    Result := Val.Value;
end;

procedure TForm6.QueueFrame(const AText: string);
begin
  if AText = '' then
    Exit;
  if not Assigned(FFrameLock) then
    Exit;

  FFrameLock.Enter;
  try
    FPendingFrame := AText;
    SetLength(FPendingFrameBytes, 0);
    FPendingFormat     := 1;
    FPendingWidth      := 0;
    FPendingHeight     := 0;
    if Assigned(FDecodeEvent) then
      FDecodeEvent.SetEvent;
  finally
    FFrameLock.Leave;
  end;
end;

procedure TForm6.QueueFrameBytes(const ABytes: TBytes; AFormat, AWidth, AHeight: Integer);
begin
  if Length(ABytes) = 0 then
    Exit;
  if not Assigned(FFrameLock) then
    Exit;

  FFrameLock.Enter;
  try
    FPendingFrame      := '';
    FPendingFrameBytes := Copy(ABytes, 0, Length(ABytes));
    FPendingFormat     := AFormat;
    FPendingWidth      := AWidth;
    FPendingHeight     := AHeight;
    if Assigned(FDecodeEvent) then
      FDecodeEvent.SetEvent;
  finally
    FFrameLock.Leave;
  end;
end;

procedure TForm6.FrameTimerTimer(Sender: TObject);
var
  RawBytes : TBytes;
  Width, Height, FrameSize: Integer;
  Bitmap   : TBitmap;
  y        : Integer;
  RowPtr   : PDWORD;
begin
  if not FCapturing then
  begin
    if Assigned(FFrameTimer) then
      FFrameTimer.Enabled := False;
    Exit;
  end;

  if TakeDecodedFrame(RawBytes, Width, Height, FrameSize) then
  begin
    Bitmap := TBitmap.Create;
    try
      Bitmap.PixelFormat := pf32bit;
      Bitmap.SetSize(Width, Height);
      for y := 0 to Height - 1 do
      begin
        RowPtr := Bitmap.ScanLine[y];
        Move(RawBytes[y * Width * 4], RowPtr^, Width * 4);
      end;
      PaintFrameBitmap(Bitmap, FrameSize);
    finally
      Bitmap.Free;
    end;
  end;

  if Assigned(FFrameTimer) then
    FFrameTimer.Enabled := FCapturing;
end;

// Pencere örtülüp açıldığında çalışır — FDisplayBitmap'ten son kareyi çizer
procedure TForm6.PaintBoxPaint(Sender: TObject);
begin
  if not Assigned(FPaintBox) then
    Exit;

  if not Assigned(FDisplayBitmap) or
     (FDisplayBitmap.Width <= 0) or
     (FDisplayBitmap.Height <= 0) then
  begin
    FPaintBox.Canvas.Brush.Color := clBlack;
    FPaintBox.Canvas.FillRect(FPaintBox.ClientRect);
    Exit;
  end;

  FPaintBox.Canvas.StretchDraw(FPaintBox.ClientRect, FDisplayBitmap);
end;

// Ana çizim rutini: Invalidate YOK → doğrudan canvas'a çiz → flicker sıfır
procedure TForm6.PaintFrameBitmap(ABitmap: TBitmap; AFrameSize: Integer);
var
  DestRect: TRect;
begin
  if not Assigned(ABitmap) or (ABitmap.Width <= 0) or (ABitmap.Height <= 0) then
    Exit;
  if not Assigned(FPaintBox) then
    Exit;

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



function TForm6.DecodeFrameToRawBGR32(const ABytes: TBytes; out ARawBytes: TBytes; out AWidth, AHeight: Integer): Boolean;
var
  Stream   : TMemoryStream;
  JpegImage: TJPEGImage;
  Picture  : TPicture;
  Graphic  : TGraphic;
  Bitmap   : TBitmap;
  y        : Integer;
  RowPtr   : PDWORD;
begin
  Result  := False;
  SetLength(ARawBytes, 0);
  AWidth  := 0;
  AHeight := 0;

  if Length(ABytes) = 0 then
    Exit;

  Stream    := TMemoryStream.Create;
  JpegImage := TJPEGImage.Create;
  Picture   := nil;
  Bitmap    := nil;
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

    Bitmap             := TBitmap.Create;
    Bitmap.PixelFormat := pf32bit;
    Bitmap.SetSize(Graphic.Width, Graphic.Height);
    Bitmap.Canvas.Draw(0, 0, Graphic);

    AWidth  := Bitmap.Width;
    AHeight := Bitmap.Height;
    SetLength(ARawBytes, AWidth * AHeight * 4);

    for y := 0 to AHeight - 1 do
    begin
      RowPtr := Bitmap.ScanLine[y];
      Move(RowPtr^, ARawBytes[y * AWidth * 4], AWidth * 4);
    end;

    Result := True;
  finally
    Bitmap.Free;
    Picture.Free;
    JpegImage.Free;
    Stream.Free;
  end;
end;

procedure TForm6.StartFrameWorker;
begin
  if Assigned(FDecodeThread) then
    Exit;

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

function TForm6.TakePendingFrame(out AText: string; out ABytes: TBytes; out AFormat, AWidth, AHeight: Integer): Boolean;
begin
  Result := False;
  AText  := '';
  SetLength(ABytes, 0);
  AFormat := 1;
  AWidth  := 0;
  AHeight := 0;

  if not Assigned(FFrameLock) then
    Exit;

  FFrameLock.Enter;
  try
    if Length(FPendingFrameBytes) > 0 then
    begin
      ABytes := FPendingFrameBytes;
      SetLength(FPendingFrameBytes, 0);
      FPendingFrame := '';
      AFormat := FPendingFormat;
      AWidth := FPendingWidth;
      AHeight := FPendingHeight;
      Result := True;
    end
    else if FPendingFrame <> '' then
    begin
      AText         := FPendingFrame;
      FPendingFrame := '';
      AFormat := FPendingFormat;
      AWidth := FPendingWidth;
      AHeight := FPendingHeight;
      Result        := True;
    end;

    if (FPendingFrame = '') and (Length(FPendingFrameBytes) = 0) and
       Assigned(FDecodeEvent) then
      FDecodeEvent.ResetEvent;
  finally
    FFrameLock.Leave;
  end;
end;

function TForm6.TakeDecodedFrame(out ARawBytes: TBytes; out AWidth, AHeight, AFrameSize: Integer): Boolean;
begin
  Result     := False;
  SetLength(ARawBytes, 0);
  AWidth     := 0;
  AHeight    := 0;
  AFrameSize := 0;

  if not Assigned(FFrameLock) then
    Exit;

  FFrameLock.Enter;
  try
    if Length(FDecodedRawBytes) > 0 then
    begin
      ARawBytes         := FDecodedRawBytes;
      AWidth            := FDecodedRawWidth;
      AHeight           := FDecodedRawHeight;
      AFrameSize        := FDecodedFrameSize;
      SetLength(FDecodedRawBytes, 0);
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
  Format   : Integer;
  Width    : Integer;
  Height   : Integer;
  RawBytes : TBytes;
  OutWidth : Integer;
  OutHeight: Integer;
  FrameSize: Integer;
begin
  CoInitialize(nil);
  try
    while not FDecodeStopping do
    begin
      if Assigned(FDecodeEvent) then
        FDecodeEvent.WaitFor(100);

      while (not FDecodeStopping) and TakePendingFrame(Text, Bytes, Format, Width, Height) do
      begin
        if (Length(Bytes) = 0) and (Text <> '') then
          DecodeBase64Image(Text, Bytes);

        FrameSize := Length(Bytes);
        SetLength(RawBytes, 0);
        try
          if Format = 2 then
          begin
            if (FDecoder = nil) or (FCurrentDecWidth <> Width) or (FCurrentDecHeight <> Height) then
            begin
              if Assigned(FDecoder) then
                FreeAndNil(FDecoder);
              FDecoder := TMFVideoDecoder.Create(Width, Height);
              FCurrentDecWidth := Width;
              FCurrentDecHeight := Height;
            end;

            if FDecoder.DecodeFrame(Bytes, RawBytes) then
            begin
              FFrameLock.Enter;
              try
                FDecodedRawBytes  := RawBytes;
                FDecodedRawWidth  := Width;
                FDecodedRawHeight := Height;
                FDecodedFrameSize := FrameSize;
              finally
                FFrameLock.Leave;
              end;
            end;
          end
          else
          begin
            if DecodeFrameToRawBGR32(Bytes, RawBytes, OutWidth, OutHeight) then
            begin
              FFrameLock.Enter;
              try
                FDecodedRawBytes  := RawBytes;
                FDecodedRawWidth  := OutWidth;
                FDecodedRawHeight := OutHeight;
                FDecodedFrameSize := FrameSize;
              finally
                FFrameLock.Leave;
              end;
            end;
          end;
        except
        end;
      end;
    end;
  finally
    if Assigned(FDecoder) then
      FreeAndNil(FDecoder);
    CoUninitialize;
  end;
end;

function TForm6.DecodeBase64Image(const AText: string; out ABytes: TBytes): Boolean;
var
  CleanText: string;
  CommaPos : Integer;
begin
  Result := False;
  SetLength(ABytes, 0);

  CleanText := Trim(AText);
  if CleanText = '' then
    Exit;

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
  if not (MonitorsVal is TJSONArray) then
    Exit;

  MonitorsArr := TJSONArray(MonitorsVal);
  if MonitorsArr.Count = 0 then
    Exit;

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
begin
  if FCapturing then
    CaptureText := 'Capturing [On]'
  else
    CaptureText := 'Capturing [Off]';

  if FLastFrameSize > 0 then
    SizeText := 'Size [' + FormatFloat('0.0 KB', FLastFrameSize / 1024) + ']'
  else
    SizeText := 'Size []';

  if StatusBar1.Panels.Count >= 2 then
  begin
    StatusBar1.Panels[0].Text := CaptureText;
    StatusBar1.Panels[1].Text := SizeText;
  end
  else
    StatusBar1.SimpleText := CaptureText + '  ' + SizeText;
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
begin
  if JSONObj = nil then
    Exit;

  UpdateMonitorList(JSONObj);

  StatusText := JSONValueText(JSONObj, 'status');
  if SameText(StatusText, 'started') then
    FCapturing := True
  else if SameText(StatusText, 'stopped') then
    FCapturing := False;

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
  if ImageText = '' then
    ImageText := JSONValueText(JSONObj, 'frame');
  if ImageText = '' then
    ImageText := JSONValueText(JSONObj, 'data');

  if ImageText <> '' then
    QueueFrame(ImageText);

  if (StatusText <> '') or (ImageText = '') then
    UpdateButtonCaption;
  if ImageText = '' then
    UpdateStatusBar;
end;

procedure TForm6.FPaintBoxMouseDown(Sender: TObject; Button: TMouseButton;
  Shift: TShiftState; X, Y: Integer);
var
  JSONObj: TJSONObject;
begin
  if not CheckBox2.Checked or not Assigned(FOnSendJSON) or not Assigned(FLine) then
    Exit;

  JSONObj := TJSONObject.Create;
  try
    JSONObj.AddPair('action', 'mouseevent');
    JSONObj.AddPair('event',  'down');
    JSONObj.AddPair('button', TJSONNumber.Create(Ord(Button))); // 0:mbLeft, 1:mbRight, 2:mbMiddle
    JSONObj.AddPair('x',      TJSONNumber.Create(Round(X * 65535 / Max(1, FPaintBox.Width))));
    JSONObj.AddPair('y',      TJSONNumber.Create(Round(Y * 65535 / Max(1, FPaintBox.Height))));
    FOnSendJSON(FLine, JSONObj);
  finally
    JSONObj.Free;
  end;
end;

procedure TForm6.FPaintBoxMouseMove(Sender: TObject; Shift: TShiftState; X,
  Y: Integer);
var
  JSONObj: TJSONObject;
  NowTick: UInt64;
begin
  if not CheckBox2.Checked or not Assigned(FOnSendJSON) or not Assigned(FLine) then
    Exit;

  NowTick := GetTickCount64;
  if (NowTick - FLastMouseMoveTick) < 30 then
    Exit;
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
  if not CheckBox2.Checked or not Assigned(FOnSendJSON) or not Assigned(FLine) then
    Exit;

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
  if not CheckBox1.Checked or not Assigned(FOnSendJSON) or not Assigned(FLine) then
    Exit;

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
  if not CheckBox1.Checked or not Assigned(FOnSendJSON) or not Assigned(FLine) then
    Exit;

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

end.
