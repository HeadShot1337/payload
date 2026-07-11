unit UnitRemoteMonitoring;

interface

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
  vpx_codec_iface_t = Pointer;
  vpx_codec_iter_t = Pointer;

  vpx_codec_ctx_t = record
    name: PAnsiChar;
    iface: Pointer;
    err: Cardinal;
    err_detail: PAnsiChar;
    init_flags: Cardinal;
    priv_dec: Pointer;
  end;

  pvpx_image_t = ^vpx_image_t;
  vpx_image_t = record
    fmt: Integer;
    w: Integer;
    h: Integer;
    d_w: Integer;
    d_h: Integer;
    x_chroma_shift: Integer;
    y_chroma_shift: Integer;
    planes: array[0..3] of PByte;
    stride: array[0..3] of Integer;
    bps: Integer;
    user_priv: Integer;
    img_data: PByte;
    img_data_owner: Integer;
    self_allocd: Integer;
    fb_priv: Pointer;
  end;

  TFn_vpx_codec_vp9_dx = function: vpx_codec_iface_t; cdecl;
  TFn_vpx_codec_dec_init_ver = function(ctx: Pointer; iface: vpx_codec_iface_t; cfg: Pointer; flags: LongInt; ver: Integer): Integer; cdecl;
  TFn_vpx_codec_decode = function(ctx: Pointer; const data: PByte; data_sz: Cardinal; user_priv: Pointer; deadline: LongInt): Integer; cdecl;
  TFn_vpx_codec_get_frame = function(ctx: Pointer; iter: Pointer): pvpx_image_t; cdecl;
  TFn_vpx_codec_destroy = function(ctx: Pointer): Integer; cdecl;

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
    FFrameLock        : TCriticalSection;
    FDecodeEvent      : TEvent;
    FDecodeThread     : TThread;
    FDecodeStopping   : Boolean;
    FDecodedBitmap    : TBitmap;
    FDecodedFrameSize : Integer;
    FDisplayBitmap    : TBitmap;         // Repaint fallback için
    FPaintBox         : TNoFlickerPaintBox; // Gerçek görüntü alanı
    FLastMouseMoveTick: UInt64;

    FVpxDecInitialized: Boolean;
    FVpxDecCtx         : vpx_codec_ctx_t;

    function  SelectedFormat: string;
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
    function  TakePendingFrame(out AText: string; out ABytes: TBytes; out AFormat: Integer): Boolean;
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
    procedure QueueFrameBytes(const ABytes: TBytes; AFormat: Integer = 1);
    procedure HandleMonitoringJSON(JSONObj: TJSONObject);
  end;

var
  Form6: TForm6;

implementation

{$R *.dfm}

var
  hVpxDll: HMODULE = 0;
  vpx_codec_vp9_dx: TFn_vpx_codec_vp9_dx = nil;
  vpx_codec_dec_init_ver: TFn_vpx_codec_dec_init_ver = nil;
  vpx_codec_decode: TFn_vpx_codec_decode = nil;
  vpx_codec_get_frame: TFn_vpx_codec_get_frame = nil;
  vpx_codec_destroy: TFn_vpx_codec_destroy = nil;

function LoadVpxDecoder: Boolean;
begin
  if hVpxDll <> 0 then
    Exit(True);

  hVpxDll := LoadLibrary('libvpx.dll');
  if hVpxDll = 0 then
    hVpxDll := LoadLibrary('vpx.dll');
  if hVpxDll = 0 then
    hVpxDll := LoadLibrary('libvpx-1.dll');

  if hVpxDll <> 0 then
  begin
    vpx_codec_vp9_dx       := GetProcAddress(hVpxDll, 'vpx_codec_vp9_dx');
    vpx_codec_dec_init_ver := GetProcAddress(hVpxDll, 'vpx_codec_dec_init_ver');
    vpx_codec_decode       := GetProcAddress(hVpxDll, 'vpx_codec_decode');
    vpx_codec_get_frame    := GetProcAddress(hVpxDll, 'vpx_codec_get_frame');
    vpx_codec_destroy      := GetProcAddress(hVpxDll, 'vpx_codec_destroy');

    if Assigned(vpx_codec_vp9_dx) and Assigned(vpx_codec_dec_init_ver) and
       Assigned(vpx_codec_decode) and Assigned(vpx_codec_get_frame) and
       Assigned(vpx_codec_destroy) then
    begin
      Exit(True);
    end;

    FreeLibrary(hVpxDll);
    hVpxDll := 0;
  end;
  Result := False;
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

procedure TForm6.QueueFrameBytes(const ABytes: TBytes; AFormat: Integer);
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

  if FVpxDecInitialized then
  begin
    if Assigned(vpx_codec_destroy) then
      vpx_codec_destroy(@FVpxDecCtx);
    FVpxDecInitialized := False;
  end;
end;

function TForm6.TakePendingFrame(out AText: string; out ABytes: TBytes; out AFormat: Integer): Boolean;
begin
  Result := False;
  AText  := '';
  SetLength(ABytes, 0);
  AFormat := 1;

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
      Result  := True;
    end
    else if FPendingFrame <> '' then
    begin
      AText         := FPendingFrame;
      FPendingFrame := '';
      AFormat       := 1;
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

procedure TForm6.DecodeFrameWorker;
var
  Text       : string;
  Bytes      : TBytes;
  Decoded    : TBitmap;
  FrameSize  : Integer;
  FrameFormat: Integer;
  Res, Ver   : Integer;
  iter       : Pointer;
  img        : pvpx_image_t;
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

    while (not FDecodeStopping) and TakePendingFrame(Text, Bytes, FrameFormat) do
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
          if not FVpxDecInitialized then
          begin
            if LoadVpxDecoder then
            begin
              Res := -1;
              for Ver := 1 to 20 do
              begin
                Res := vpx_codec_dec_init_ver(@FVpxDecCtx, vpx_codec_vp9_dx(), nil, 0, Ver);
                if Res = 0 then
                  Break;
              end;
              if Res = 0 then
                FVpxDecInitialized := True;
            end;
          end;

          if FVpxDecInitialized then
          begin
            Res := vpx_codec_decode(@FVpxDecCtx, @Bytes[0], FrameSize, nil, 0);
            if Res = 0 then
            begin
              iter := nil;
              img := vpx_codec_get_frame(@FVpxDecCtx, @iter);
              if img <> nil then
              begin
                Decoded := TBitmap.Create;
                Decoded.PixelFormat := pf24bit;
                Decoded.SetSize(img.w, img.h);

                for y := 0 to img.h - 1 do
                begin
                  rowPtr := Decoded.ScanLine[y];
                  for x := 0 to img.w - 1 do
                  begin
                    YVal := img.planes[0][y * img.stride[0] + x];
                    UVal := img.planes[1][(y div 2) * img.stride[1] + (x div 2)];
                    VVal := img.planes[2][(y div 2) * img.stride[2] + (x div 2)];

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
              end;
            end;
          end;
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
