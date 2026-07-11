unit UnitRemoteMonitoring;

interface

{$POINTERMATH ON}

uses
  Winapi.Windows, Winapi.Messages,
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

type
  // Standard Media Foundation interface declarations
  IMFCollection = interface;
  IMFMediaBuffer = interface;
  IMFSample = interface;
  IMFMediaType = interface;
  IMFTransform = interface;

  IMFMediaBuffer = interface(IUnknown)
    ['{c40a00f2-b93a-4d80-ae90-aa3c10a48472}']
    function Lock(out ppbBuffer: PByte; out pcbMaxLength: Cardinal; out pcbCurrentLength: Cardinal): HRESULT; stdcall;
    function Unlock: HRESULT; stdcall;
    function GetCurrentLength(out pcbCurrentLength: Cardinal): HRESULT; stdcall;
    function SetCurrentLength(cbCurrentLength: Cardinal): HRESULT; stdcall;
    function GetMaxLength(out pcbMaxLength: Cardinal): HRESULT; stdcall;
  end;

  IMFCollection = interface(IUnknown)
    ['{5bc8a76b-869a-46a3-9b03-fa218a66aebe}']
    function GetElementCount(out pcElements: Cardinal): HRESULT; stdcall;
    function GetElement(dwElementIndex: Cardinal; out ppUnkElement: IUnknown): HRESULT; stdcall;
    function AddElement(const pUnkElement: IUnknown): HRESULT; stdcall;
    function RemoveElement(dwElementIndex: Cardinal; out ppUnkElement: IUnknown): HRESULT; stdcall;
    function InsertElementAt(dwIndex: Cardinal; const pUnkElement: IUnknown): HRESULT; stdcall;
    function RemoveAllElements: HRESULT; stdcall;
  end;

  IMFSample = interface(IUnknown)
    ['{c40a00f1-b93a-4d80-ae90-aa3c10a48472}']
    // IMFAttributes (Parent methods inside IMFSample vtable)
    function GetItem(const guidKey: TGUID; pValue: Pointer): HRESULT; stdcall;
    function GetItemType(const guidKey: TGUID; out pType: Integer): HRESULT; stdcall;
    function CompareItem(const guidKey: TGUID; const Value: Pointer; out pbResult: Boolean): HRESULT; stdcall;
    function Compare(const pTheirs: Pointer; out pbResult: Boolean): HRESULT; stdcall;
    function GetUINT32(const guidKey: TGUID; out punValue: Cardinal): HRESULT; stdcall;
    function GetUINT64(const guidKey: TGUID; out punValue: UInt64): HRESULT; stdcall;
    function GetDouble(const guidKey: TGUID; out pdblValue: Double): HRESULT; stdcall;
    function GetGUID(const guidKey: TGUID; out pguidValue: TGUID): HRESULT; stdcall;
    function GetStringLength(const guidKey: TGUID; out pcchLength: Cardinal): HRESULT; stdcall;
    function GetString(const guidKey: TGUID; pwszValue: PWideChar; cchBufSize: Cardinal; out pcchLength: Cardinal): HRESULT; stdcall;
    function GetAllocatedString(const guidKey: TGUID; out ppwszValue: PWideChar; out pcchLength: Cardinal): HRESULT; stdcall;
    function GetBlobSize(const guidKey: TGUID; out pcbBlobSize: Cardinal): HRESULT; stdcall;
    function GetBlob(const guidKey: TGUID; pBuf: PByte; cbBufSize: Cardinal; out pcbBlobSize: Cardinal): HRESULT; stdcall;
    function GetAllocatedBlob(const guidKey: TGUID; out ppBuf: PByte; out pcbSize: Cardinal): HRESULT; stdcall;
    function GetUnknown(const guidKey: TGUID; const riid: TGUID; out ppvObject): HRESULT; stdcall;
    function SetItem(const guidKey: TGUID; const Value: Pointer): HRESULT; stdcall;
    function DeleteItem(const guidKey: TGUID): HRESULT; stdcall;
    function DeleteAllItems: HRESULT; stdcall;
    function SetUINT32(const guidKey: TGUID; unValue: Cardinal): HRESULT; stdcall;
    function SetUINT64(const guidKey: TGUID; unValue: UInt64): HRESULT; stdcall;
    function SetDouble(const guidKey: TGUID; dblValue: Double): HRESULT; stdcall;
    function SetGUID(const guidKey: TGUID; const guidValue: TGUID): HRESULT; stdcall;
    function SetString(const guidKey: TGUID; pwszValue: PWideChar): HRESULT; stdcall;
    function SetBlob(const guidKey: TGUID; const pBuf: PByte; cbBufSize: Cardinal): HRESULT; stdcall;
    function SetUnknown(const guidKey: TGUID; const pUnknown: IUnknown): HRESULT; stdcall;
    function LockStore: HRESULT; stdcall;
    function UnlockStore: HRESULT; stdcall;
    function GetCount(out pcItems: Cardinal): HRESULT; stdcall;
    function GetItemByIndex(unIndex: Cardinal; out pguidKey: TGUID; pValue: Pointer): HRESULT; stdcall;
    function CopyAllItems(const pDest: Pointer): HRESULT; stdcall;

    // IMFSample methods
    function GetSampleFlags(out pdwFlags: Cardinal): HRESULT; stdcall;
    function SetSampleFlags(dwFlags: Cardinal): HRESULT; stdcall;
    function GetSampleTime(out phnsSampleTime: Int64): HRESULT; stdcall;
    function SetSampleTime(hnsSampleTime: Int64): HRESULT; stdcall;
    function GetSampleDuration(out phnsSampleDuration: Int64): HRESULT; stdcall;
    function SetSampleDuration(hnsSampleDuration: Int64): HRESULT; stdcall;
    function GetBufferCount(out pdwBufferCount: Cardinal): HRESULT; stdcall;
    function GetBufferByIndex(dwIndex: Cardinal; out ppBuffer: IMFMediaBuffer): HRESULT; stdcall;
    function ConvertToContiguousBuffer(out ppBuffer: IMFMediaBuffer): HRESULT; stdcall;
    function AddBuffer(const pBuffer: IMFMediaBuffer): HRESULT; stdcall;
    function RemoveBufferByIndex(dwIndex: Cardinal): HRESULT; stdcall;
    function RemoveAllBuffers: HRESULT; stdcall;
    function GetTotalLength(out pcbTotalLength: Cardinal): HRESULT; stdcall;
    function CopyAllSamples(const pDestSample: IMFSample): HRESULT; stdcall;
  end;

  IMFMediaType = interface(IUnknown)
    ['{04ea0fa8-ea31-4109-8d24-674f9e3efb76}']
    // IMFAttributes (Parent methods inside IMFMediaType vtable)
    function GetItem(const guidKey: TGUID; pValue: Pointer): HRESULT; stdcall;
    function GetItemType(const guidKey: TGUID; out pType: Integer): HRESULT; stdcall;
    function CompareItem(const guidKey: TGUID; const Value: Pointer; out pbResult: Boolean): HRESULT; stdcall;
    function Compare(const pTheirs: Pointer; out pbResult: Boolean): HRESULT; stdcall;
    function GetUINT32(const guidKey: TGUID; out punValue: Cardinal): HRESULT; stdcall;
    function GetUINT64(const guidKey: TGUID; out punValue: UInt64): HRESULT; stdcall;
    function GetDouble(const guidKey: TGUID; out pdblValue: Double): HRESULT; stdcall;
    function GetGUID(const guidKey: TGUID; out pguidValue: TGUID): HRESULT; stdcall;
    function GetStringLength(const guidKey: TGUID; out pcchLength: Cardinal): HRESULT; stdcall;
    function GetString(const guidKey: TGUID; pwszValue: PWideChar; cchBufSize: Cardinal; out pcchLength: Cardinal): HRESULT; stdcall;
    function GetAllocatedString(const guidKey: TGUID; out ppwszValue: PWideChar; out pcchLength: Cardinal): HRESULT; stdcall;
    function GetBlobSize(const guidKey: TGUID; out pcbBlobSize: Cardinal): HRESULT; stdcall;
    function GetBlob(const guidKey: TGUID; pBuf: PByte; cbBufSize: Cardinal; out pcbBlobSize: Cardinal): HRESULT; stdcall;
    function GetAllocatedBlob(const guidKey: TGUID; out ppBuf: PByte; out pcbSize: Cardinal): HRESULT; stdcall;
    function GetUnknown(const guidKey: TGUID; const riid: TGUID; out ppvObject): HRESULT; stdcall;
    function SetItem(const guidKey: TGUID; const Value: Pointer): HRESULT; stdcall;
    function DeleteItem(const guidKey: TGUID): HRESULT; stdcall;
    function DeleteAllItems: HRESULT; stdcall;
    function SetUINT32(const guidKey: TGUID; unValue: Cardinal): HRESULT; stdcall;
    function SetUINT64(const guidKey: TGUID; unValue: UInt64): HRESULT; stdcall;
    function SetDouble(const guidKey: TGUID; dblValue: Double): HRESULT; stdcall;
    function SetGUID(const guidKey: TGUID; const guidValue: TGUID): HRESULT; stdcall;
    function SetString(const guidKey: TGUID; pwszValue: PWideChar): HRESULT; stdcall;
    function SetBlob(const guidKey: TGUID; const pBuf: PByte; cbBufSize: Cardinal): HRESULT; stdcall;
    function SetUnknown(const guidKey: TGUID; const pUnknown: IUnknown): HRESULT; stdcall;
    function LockStore: HRESULT; stdcall;
    function UnlockStore: HRESULT; stdcall;
    function GetCount(out pcItems: Cardinal): HRESULT; stdcall;
    function GetItemByIndex(unIndex: Cardinal; out pguidKey: TGUID; pValue: Pointer): HRESULT; stdcall;
    function CopyAllItems(const pDest: Pointer): HRESULT; stdcall;

    // IMFMediaType methods
    function GetMajorType(out pguidMajorType: TGUID): HRESULT; stdcall;
    function IsCompressedFormat(out pfCompressed: Boolean): HRESULT; stdcall;
    function IsEqual(const pIMediaType: IMFMediaType; out pdwFlags: Cardinal): HRESULT; stdcall;
    function GetRepresentation(guidRepresentation: TGUID; out ppvRepresentation: Pointer): HRESULT; stdcall;
    function FreeRepresentation(guidRepresentation: TGUID; pvRepresentation: Pointer): HRESULT; stdcall;
  end;

  IMFTransform = interface(IUnknown)
    ['{bf94c121-5b05-4e6f-8000-ba598961414d}']
    function GetStreamLimits(out pdwInputMinimum: Cardinal; out pdwInputMaximum: Cardinal; out pdwOutputMinimum: Cardinal; out pdwOutputMaximum: Cardinal): HRESULT; stdcall;
    function GetStreamCount(out pdwInputStreams: Cardinal; out pdwOutputStreams: Cardinal): HRESULT; stdcall;
    function GetStreamIDs(dwInputIDArraySize: Cardinal; out pdwInputIDs: Cardinal; dwOutputIDArraySize: Cardinal; out pdwOutputIDs: Cardinal): HRESULT; stdcall;
    function GetInputStreamInfo(dwInputStreamID: Cardinal; out pStreamInfo: Pointer): HRESULT; stdcall;
    function GetOutputStreamInfo(dwOutputStreamID: Cardinal; out pStreamInfo: Pointer): HRESULT; stdcall;
    function GetAttributes(out ppAttributes: Pointer): HRESULT; stdcall;
    function GetInputStreamAttributes(dwInputStreamID: Cardinal; out ppAttributes: Pointer): HRESULT; stdcall;
    function GetOutputStreamAttributes(dwOutputStreamID: Cardinal; out ppAttributes: Pointer): HRESULT; stdcall;
    function DeleteInputStream(dwStreamID: Cardinal): HRESULT; stdcall;
    function AddInputStreams(cStreams: Cardinal; out pdwStreamIDs: Cardinal): HRESULT; stdcall;
    function GetInputAvailableType(dwInputStreamID: Cardinal; dwTypeIndex: Cardinal; out ppType: IMFMediaType): HRESULT; stdcall;
    function GetOutputAvailableType(dwOutputStreamID: Cardinal; dwTypeIndex: Cardinal; out ppType: IMFMediaType): HRESULT; stdcall;
    function SetInputType(dwInputStreamID: Cardinal; const pType: IMFMediaType; dwFlags: Cardinal): HRESULT; stdcall;
    function SetOutputType(dwOutputStreamID: Cardinal; const pType: IMFMediaType; dwFlags: Cardinal): HRESULT; stdcall;
    function GetInputCurrentType(dwInputStreamID: Cardinal; out ppType: IMFMediaType): HRESULT; stdcall;
    function GetOutputCurrentType(dwOutputStreamID: Cardinal; out ppType: IMFMediaType): HRESULT; stdcall;
    function GetInputStatus(dwInputStreamID: Cardinal; out pdwFlags: Cardinal): HRESULT; stdcall;
    function GetOutputStatus(out pdwFlags: Cardinal): HRESULT; stdcall;
    function SetOutputBounds(hnsLowerBound: Int64; hnsUpperBound: Int64): HRESULT; stdcall;
    function ProcessMessage(eMessage: Cardinal; ulParam: NativeUInt): HRESULT; stdcall;
    function ProcessInput(dwInputStreamID: Cardinal; const pSample: IMFSample; dwFlags: Cardinal): HRESULT; stdcall;
    function ProcessOutput(dwFlags: Cardinal; cOutputBufferCount: Cardinal; pOutputSamples: Pointer; out pdwStatus: Cardinal): HRESULT; stdcall;
  end;

  _MFT_OUTPUT_DATA_BUFFER = record
    dwStreamID: Cardinal;
    pSample: IMFSample;
    dwStatus: Cardinal;
    pEvents: IMFCollection;
  end;
  MFT_OUTPUT_DATA_BUFFER = _MFT_OUTPUT_DATA_BUFFER;

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
    ComboBox3: TComboBox;
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
    FPendingFrameFormat: Integer;
    FPendingWidth      : Integer;
    FPendingHeight     : Integer;
    FCurrentWidth      : Integer;
    FCurrentHeight     : Integer;
    FFrameLock        : TCriticalSection;
    FDecodeEvent      : TEvent;
    FDecodeThread     : TThread;
    FDecodeStopping   : Boolean;
    FDecodedBitmap    : TBitmap;
    FDecodedFrameSize : Integer;
    FDisplayBitmap    : TBitmap;         // Repaint fallback için
    FPaintBox         : TNoFlickerPaintBox; // Gerçek görüntü alanı
    FLastMouseMoveTick: UInt64;

    FWmfDecInitialized: Boolean;
    FWmfDecoder        : IMFTransform;

    function  SelectedFormat: string;
    function  InitWmfDecoder(AWidth, AHeight: Integer): Boolean;
    procedure FillDefaultOptions;
    procedure SendMonitoringCommand(const AAction: string);
    function  SelectedMonitorIndex: Integer;
    function  SelectedScalePercent: Integer;
    function  JSONValueText(JSONObj: TJSONObject; const AName: string): string;
    function  DecodeBase64Image(const AText: string; out ABytes: TBytes): Boolean;
    function  DecodeFrameToBitmap(const ABytes: TBytes; out ABitmap: TBitmap): Boolean;
    procedure QueueFrame(const AText: string);
    procedure FrameTimerTimer(Sender: TObject);
    procedure PaintBoxPaint(Sender: TObject);
    procedure PaintFrameBitmap(ABitmap: TBitmap; AFrameSize: Integer);
    procedure StartFrameWorker;
    procedure StopFrameWorker;
    procedure DecodeFrameWorker;
    function  TakePendingFrame(out AText: string; out ABytes: TBytes; out AFormat: Integer; out AWidth, AHeight: Integer): Boolean;
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
    procedure QueueFrameBytes(const ABytes: TBytes; AFormat: Integer = 1; AWidth: Integer = 1280; AHeight: Integer = 720);
    procedure HandleMonitoringJSON(JSONObj: TJSONObject);
  end;

var
  Form6: TForm6;

implementation

{$R *.dfm}

const
  CLSID_CMSVP9DecoderMFT: TGUID = '{65a5ca6e-0a06-4448-831b-7359929283fa}';
  MFVideoFormat_VP9: TGUID = '{63647076-0000-0010-8000-00aa00389b71}';
  MFVideoFormat_NV12: TGUID = '{00000015-0000-0010-8000-00aa00389b71}';
  MF_MT_MAJOR_TYPE: TGUID = '{48eba18e-f8c9-4687-bf11-0a74c9f96a8f}';
  MF_MT_SUBTYPE: TGUID = '{f7e34c9a-425f-4e15-aa3c-5a224d3fd0bc}';
  MF_MT_FRAME_SIZE: TGUID = '{166177ae-3341-4ee4-9721-a3f402bab995}';
  MFMediaType_Video: TGUID = '{73646976-0000-0010-8000-00aa00389b71}';
  MFT_MESSAGE_COMMAND_FLUSH = 0;
  MFT_MESSAGE_NOTIFY_BEGIN_STREAMING = $10000000;
  MFT_MESSAGE_NOTIFY_START_OF_STREAM = $10000003;
  MF_E_TRANSFORM_NEED_MORE_INPUT = $C00D6D72;
  MF_VERSION = (1 shl 16) or 7;

var
  hMfPlatDll: HMODULE = 0;
  MFStartup: function(Version: Cardinal; dwFlags: Cardinal = 0): HRESULT; stdcall = nil;
  MFShutdown: function: HRESULT; stdcall = nil;
  MFCreateMediaType: function(out ppMFType: IMFMediaType): HRESULT; stdcall = nil;
  MFCreateMemoryBuffer: function(cbMaxLength: Cardinal; out ppBuffer: IMFMediaBuffer): HRESULT; stdcall = nil;
  MFCreateSample: function(out ppSample: IMFSample): HRESULT; stdcall = nil;

function LoadWmfDecoder: Boolean;
begin
  if hMfPlatDll <> 0 then
    Exit(True);

  hMfPlatDll := LoadLibrary('mfplat.dll');
  if hMfPlatDll <> 0 then
  begin
    @MFStartup            := GetProcAddress(hMfPlatDll, 'MFStartup');
    @MFShutdown           := GetProcAddress(hMfPlatDll, 'MFShutdown');
    @MFCreateMediaType    := GetProcAddress(hMfPlatDll, 'MFCreateMediaType');
    @MFCreateMemoryBuffer := GetProcAddress(hMfPlatDll, 'MFCreateMemoryBuffer');
    @MFCreateSample       := GetProcAddress(hMfPlatDll, 'MFCreateSample');

    if Assigned(MFStartup) and Assigned(MFShutdown) and
       Assigned(MFCreateMediaType) and Assigned(MFCreateMemoryBuffer) and
       Assigned(MFCreateSample) then
    begin
      Exit(True);
    end;

    FreeLibrary(hMfPlatDll);
    hMfPlatDll := 0;
  end;
  Result := False;
end;

function MFSetAttributeSize(const pType: IMFMediaType; const guidKey: TGUID; unWidth, unHeight: Cardinal): HRESULT;
begin
  Result := pType.SetUINT64(guidKey, (Int64(unWidth) shl 32) or unHeight);
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
  FLine             := aLine;
  FClientID         := AClientID;
  FOnSendJSON       := ASendJSON;
  FOnFormClosed     := AFormClosed;
  FCapturing        := False;
  FLastFrameSize    := 0;
  FLastStatusTick   := 0;
  FPendingFrame     := '';
  SetLength(FPendingFrameBytes, 0);
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
    ComboBox3.OnChange := ComboBox3Change;

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

  if Assigned(ComboBox3) then
  begin
    if ComboBox3.Items.Count = 0 then
    begin
      ComboBox3.Items.Add('JPEG');
      ComboBox3.Items.Add('VP9');
    end;
    if ComboBox3.ItemIndex < 0 then
      ComboBox3.ItemIndex := 0;
  end;
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
    JSONObj.AddPair('format',  SelectedFormat);
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

procedure TForm6.ComboBox3Change(Sender: TObject);
begin
  if FCapturing then
    SendMonitoringCommand('monitorstart');
end;

function TForm6.SelectedFormat: string;
begin
  if Assigned(ComboBox3) and (ComboBox3.ItemIndex >= 0) then
    Result := ComboBox3.Items[ComboBox3.ItemIndex]
  else
    Result := 'JPEG';
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
    if Assigned(FDecodeEvent) then
      FDecodeEvent.SetEvent;
  finally
    FFrameLock.Leave;
  end;
end;

procedure TForm6.QueueFrameBytes(const ABytes: TBytes; AFormat: Integer; AWidth: Integer; AHeight: Integer);
begin
  if Length(ABytes) = 0 then
    Exit;
  if not Assigned(FFrameLock) then
    Exit;

  FFrameLock.Enter;
  try
    FPendingFrame       := '';
    FPendingFrameBytes  := Copy(ABytes, 0, Length(ABytes));
    FPendingFrameFormat := AFormat;
    FPendingWidth       := AWidth;
    FPendingHeight      := AHeight;
    if Assigned(FDecodeEvent) then
      FDecodeEvent.SetEvent;
  finally
    FFrameLock.Leave;
  end;
end;

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

function TForm6.DecodeFrameToBitmap(const ABytes: TBytes; out ABitmap: TBitmap): Boolean;
var
  Stream   : TMemoryStream;
  JpegImage: TJPEGImage;
  Picture  : TPicture;
  Graphic  : TGraphic;
begin
  Result  := False;
  ABitmap := nil;

  if Length(ABytes) = 0 then
    Exit;

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
    if not Result then
      FreeAndNil(ABitmap);
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

  if FWmfDecInitialized then
  begin
    if FWmfDecoder <> nil then
    begin
      FWmfDecoder.ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
      FWmfDecoder := nil;
    end;
    MFShutdown;
    FWmfDecInitialized := False;
  end;
end;

function TForm6.TakePendingFrame(out AText: string; out ABytes: TBytes; out AFormat: Integer; out AWidth, AHeight: Integer): Boolean;
begin
  Result := False;
  AText  := '';
  SetLength(ABytes, 0);
  AFormat := 1;
  AWidth := 1280;
  AHeight := 720;

  if not Assigned(FFrameLock) then
    Exit;

  FFrameLock.Enter;
  try
    if Length(FPendingFrameBytes) > 0 then
    begin
      ABytes  := FPendingFrameBytes;
      SetLength(FPendingFrameBytes, 0);
      FPendingFrame := '';
      AFormat := FPendingFrameFormat;
      AWidth  := FPendingWidth;
      AHeight := FPendingHeight;
      Result  := True;
    end
    else if FPendingFrame <> '' then
    begin
      AText         := FPendingFrame;
      FPendingFrame := '';
      AFormat       := 1;
      AWidth        := 1280;
      AHeight       := 720;
      Result        := True;
    end;

    if (FPendingFrame = '') and (Length(FPendingFrameBytes) = 0) and
       Assigned(FDecodeEvent) then
      FDecodeEvent.ResetEvent;
  finally
    FFrameLock.Leave;
  end;
end;

function TForm6.TakeDecodedFrame(out ABitmap: TBitmap; out AFrameSize: Integer): Boolean;
begin
  Result     := False;
  ABitmap    := nil;
  AFrameSize := 0;

  if not Assigned(FFrameLock) then
    Exit;

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

function TForm6.InitWmfDecoder(AWidth, AHeight: Integer): Boolean;
var
  hr: HRESULT;
  pInputType, pOutputType: IMFMediaType;
begin
  Result := False;
  if FWmfDecInitialized then
    Exit(True);

  if not LoadWmfDecoder then
    Exit;

  hr := CoInitializeEx(nil, COINIT_APARTMENTTHREADED);
  hr := MFStartup(MF_VERSION, 0);
  if Failed(hr) then
    Exit;

  hr := CoCreateInstance(CLSID_CMSVP9DecoderMFT, nil, CLSCTX_INPROC_SERVER, IMFTransform, FWmfDecoder);
  if Failed(hr) or (FWmfDecoder = nil) then
  begin
    MFShutdown;
    Exit;
  end;

  hr := MFCreateMediaType(pInputType);
  if Succeeded(hr) then
  begin
    pInputType.SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pInputType.SetGUID(MF_MT_SUBTYPE, MFVideoFormat_VP9);
    MFSetAttributeSize(pInputType, MF_MT_FRAME_SIZE, AWidth, AHeight);
    hr := FWmfDecoder.SetInputType(0, pInputType, 0);
  end;

  if Failed(hr) then
  begin
    FWmfDecoder := nil;
    MFShutdown;
    Exit;
  end;

  hr := MFCreateMediaType(pOutputType);
  if Succeeded(hr) then
  begin
    pOutputType.SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pOutputType.SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(pOutputType, MF_MT_FRAME_SIZE, AWidth, AHeight);
    hr := FWmfDecoder.SetOutputType(0, pOutputType, 0);
  end;

  if Failed(hr) then
  begin
    FWmfDecoder := nil;
    MFShutdown;
    Exit;
  end;

  hr := FWmfDecoder.ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
  hr := FWmfDecoder.ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  hr := FWmfDecoder.ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

  FCurrentWidth := AWidth;
  FCurrentHeight := AHeight;
  FWmfDecInitialized := True;
  Result := True;
end;

procedure TForm6.DecodeFrameWorker;
var
  Text       : string;
  Bytes      : TBytes;
  Decoded    : TBitmap;
  FrameSize  : Integer;
  FrameFormat: Integer;
  FrameWidth : Integer;
  FrameHeight: Integer;
  hr         : HRESULT;
  pBuffer    : IMFMediaBuffer;
  pData      : PByte;
  dwMaxLen   : Cardinal;
  dwCurrentLen: Cardinal;
  pSample    : IMFSample;
  outputBuffer: MFT_OUTPUT_DATA_BUFFER;
  pOutputSample: IMFSample;
  pOutputBuffer: IMFMediaBuffer;
  dwStatus   : Cardinal;
  pOutData   : PByte;
  dwOutLen   : Cardinal;
  YPlane, UVPlane: PByte;
  y, x       : Integer;
  rowPtr     : PByte;
  YVal, UVal, VVal : Byte;
  CVal, DVal, EVal : Integer;
  RVal, GVal, BVal : Integer;
begin
  while not FDecodeStopping do
  begin
    if Assigned(FDecodeEvent) then
      FDecodeEvent.WaitFor(100);

    while (not FDecodeStopping) and TakePendingFrame(Text, Bytes, FrameFormat, FrameWidth, FrameHeight) do
    begin
      if (Length(Bytes) = 0) and (Text <> '') then
      begin
        DecodeBase64Image(Text, Bytes);
        FrameFormat := 1;
      end;

      FrameSize := Length(Bytes);
      if FrameSize = 0 then
        Continue;

      Decoded := nil;
      try
        if FrameFormat = 4 then
        begin
          if FWmfDecInitialized and ((FCurrentWidth <> FrameWidth) or (FCurrentHeight <> FrameHeight)) then
          begin
            FWmfDecoder.ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
            FWmfDecoder := nil;
            FWmfDecInitialized := False;
          end;

          if InitWmfDecoder(FrameWidth, FrameHeight) then
          begin
            hr := MFCreateMemoryBuffer(FrameSize, pBuffer);
            if Succeeded(hr) then
            begin
              hr := pBuffer.Lock(pData, dwMaxLen, dwCurrentLen);
              if Succeeded(hr) then
              begin
                Move(Bytes[0], pData^, FrameSize);
                pBuffer.Unlock;
                pBuffer.SetCurrentLength(FrameSize);
              end;

              hr := MFCreateSample(pSample);
              if Succeeded(hr) then
              begin
                pSample.AddBuffer(pBuffer);
                hr := FWmfDecoder.ProcessInput(0, pSample, 0);
                if Succeeded(hr) then
                begin
                  outputBuffer.dwStreamID := 0;
                  outputBuffer.pSample := nil;
                  outputBuffer.dwStatus := 0;
                  outputBuffer.pEvents := nil;

                  hr := MFCreateSample(pOutputSample);
                  if Succeeded(hr) then
                  begin
                    hr := MFCreateMemoryBuffer(FrameWidth * FrameHeight * 3 div 2, pOutputBuffer);
                    if Succeeded(hr) then
                    begin
                      pOutputSample.AddBuffer(pOutputBuffer);
                      outputBuffer.pSample := pOutputSample;

                      dwStatus := 0;
                      hr := FWmfDecoder.ProcessOutput(0, 1, @outputBuffer, dwStatus);
                      if Succeeded(hr) then
                      begin
                        hr := outputBuffer.pSample.GetBufferByIndex(0, pOutputBuffer);
                        if Succeeded(hr) then
                        begin
                          hr := pOutputBuffer.Lock(pOutData, dwMaxLen, dwOutLen);
                          if Succeeded(hr) then
                          begin
                            var pActiveType: IMFMediaType;
                            var unW, unH: Cardinal;
                            unW := 0; unH := 0;
                            hr := FWmfDecoder.GetOutputCurrentType(0, pActiveType);
                            if Succeeded(hr) and (pActiveType <> nil) then
                            begin
                              var sizeVal: UInt64;
                              sizeVal := 0;
                              if Succeeded(pActiveType.GetUINT64(MF_MT_FRAME_SIZE, sizeVal)) then
                              begin
                                unW := sizeVal shr 32;
                                unH := sizeVal and $FFFFFFFF;
                              end;
                            end;

                            if (unW <= 0) or (unH <= 0) then
                            begin
                              unW := FrameWidth;
                              unH := FrameHeight;
                            end;

                            Decoded := TBitmap.Create;
                            Decoded.PixelFormat := pf24bit;
                            Decoded.SetSize(unW, unH);

                            YPlane  := pOutData;
                            UVPlane := pOutData + (unW * unH);

                            for y := 0 to Integer(unH) - 1 do
                            begin
                              rowPtr := Decoded.ScanLine[y];
                              for x := 0 to Integer(unW) - 1 do
                              begin
                                YVal := YPlane[y * Integer(unW) + x];
                                UVal := UVPlane[(y div 2) * Integer(unW) + (x div 2) * 2];
                                VVal := UVPlane[(y div 2) * Integer(unW) + (x div 2) * 2 + 1];

                                CVal := YVal - 16;
                                DVal := UVal - 128;
                                EVal := VVal - 128;

                                RVal := (298 * CVal             + 409 * EVal + 128) shr 8;
                                GVal := (298 * CVal - 100 * DVal - 208 * EVal + 128) shr 8;
                                BVal := (298 * CVal + 516 * DVal             + 128) shr 8;

                                if RVal < 0 then RVal := 0 else if RVal > 255 then RVal := 255;
                                if GVal < 0 then GVal := 0 else if GVal > 255 then GVal := 255;
                                if BVal < 0 then BVal := 0 else if BVal > 255 then BVal := 255;

                                rowPtr[x * 3]     := BVal;
                                rowPtr[x * 3 + 1] := GVal;
                                rowPtr[x * 3 + 2] := RVal;
                              end;
                            end;

                            pOutputBuffer.Unlock;
                          end;
                        end;
                      end;
                    end;
                  end;
                end;
              end;
            end;
          end;
          outputBuffer.pSample := nil;
          outputBuffer.pEvents := nil;
          pOutputSample := nil;
          pOutputBuffer := nil;
          pSample := nil;
          pBuffer := nil;
        end
        else
        begin
          DecodeFrameToBitmap(Bytes, Decoded);
        end;

        if Assigned(Decoded) then
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
