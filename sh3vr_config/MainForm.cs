using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Windows.Forms;

namespace SH3VRConfig
{
    internal enum IniTarget { Main, Weapons }
    internal enum ValueKind { Boolean, Integer, Decimal, Choice }

    internal sealed class SettingBinding
    {
        public IniTarget Target;
        public string Section;
        public string Key;
        public string DefaultValue;
        public ValueKind Kind;
        public Control Control;
        public Dictionary<string, string> Choices;
    }

    internal sealed class MainForm : Form
    {
        private readonly string gameDirectory = AppDomain.CurrentDomain.BaseDirectory;
        private readonly string mainIniPath;
        private readonly string weaponsIniPath;
        private IniDocument mainIni;
        private IniDocument weaponsIni;
        private bool configurationFilesAvailable;
        private readonly List<SettingBinding> bindings = new List<SettingBinding>();
        private readonly Label descriptionTitle = new Label();
        private readonly Label descriptionText = new Label();
        private readonly Label statusLabel = new Label();
        private readonly ComboBox resolutionPreset = new ComboBox();
        private readonly NumericUpDown eyeWidth = new NumericUpDown();
        private readonly NumericUpDown eyeHeight = new NumericUpDown();
        private readonly ComboBox refreshRate = new ComboBox();

        public MainForm()
        {
            mainIniPath = Path.Combine(gameDirectory, "sh3vr.ini");
            weaponsIniPath = Path.Combine(gameDirectory, "sh3vr_weapons.ini");
            Text = "Silent Hill 3 VR Configuration Tool";
            Icon = SystemIcons.Application;
            StartPosition = FormStartPosition.CenterScreen;
            MinimumSize = new Size(920, 680);
            ClientSize = new Size(990, 760);
            Font = new Font("Segoe UI", 9F);

            var root = new TableLayoutPanel { Dock = DockStyle.Fill, RowCount = 4, ColumnCount = 1 };
            root.RowStyles.Add(new RowStyle(SizeType.Percent, 100F));
            root.RowStyles.Add(new RowStyle(SizeType.Absolute, 112F));
            root.RowStyles.Add(new RowStyle(SizeType.Absolute, 26F));
            root.RowStyles.Add(new RowStyle(SizeType.Absolute, 44F));
            Controls.Add(root);

            var tabs = new TabControl { Dock = DockStyle.Fill, Padding = new Point(14, 5) };
            root.Controls.Add(tabs, 0, 0);
            tabs.TabPages.Add(BuildDisplayTab());
            tabs.TabPages.Add(BuildInputTab());
            tabs.TabPages.Add(BuildGraphicsTab());
            tabs.TabPages.Add(BuildAdvancedTab());
            tabs.TabPages.Add(BuildMotionTab());
            tabs.TabPages.Add(BuildAboutTab());

            var descriptionPanel = new Panel { Dock = DockStyle.Fill, BorderStyle = BorderStyle.FixedSingle };
            descriptionTitle.Dock = DockStyle.Top;
            descriptionTitle.Height = 28;
            descriptionTitle.Font = new Font(Font, FontStyle.Bold);
            descriptionTitle.Padding = new Padding(7, 5, 7, 0);
            descriptionText.Dock = DockStyle.Fill;
            descriptionText.Padding = new Padding(7, 2, 7, 5);
            descriptionPanel.Controls.Add(descriptionText);
            descriptionPanel.Controls.Add(descriptionTitle);
            root.Controls.Add(descriptionPanel, 0, 1);

            statusLabel.Dock = DockStyle.Fill;
            statusLabel.TextAlign = ContentAlignment.MiddleLeft;
            statusLabel.Padding = new Padding(7, 0, 0, 0);
            statusLabel.ForeColor = Color.DimGray;
            root.Controls.Add(statusLabel, 0, 2);

            var buttons = new FlowLayoutPanel {
                Dock = DockStyle.Fill, FlowDirection = FlowDirection.RightToLeft,
                Padding = new Padding(5), WrapContents = false
            };
            buttons.Controls.Add(MakeButton("Save & Launch Game", 170, delegate { SaveAndLaunch(); }));
            buttons.Controls.Add(MakeButton("Save", 110, delegate { SaveSettings(); }));
            buttons.Controls.Add(MakeButton("Defaults", 110, delegate { LoadDefaults(); }));
            buttons.Controls.Add(MakeButton("Close", 110, delegate { Close(); }));
            root.Controls.Add(buttons, 0, 3);
            Shown += delegate { LoadSettings(); };
        }

        private TabPage BuildDisplayTab()
        {
            var page = NewPage("Display");
            var panel = NewSettingsPanel();
            page.Controls.Add(panel);
            var resolution = NewGroup("VR Resolution", 150);
            panel.Controls.Add(resolution);
            var grid = NewGrid(3);
            resolution.Controls.Add(grid);
            ConfigureCombo(resolutionPreset, new[] {
                "Runtime recommended", "1900 x 2076", "2064 x 2208", "2496 x 2688", "Custom"
            });
            ConfigureNumber(eyeWidth, 0, 8192, 1, 0);
            ConfigureNumber(eyeHeight, 0, 8192, 1, 0);
            resolutionPreset.SelectedIndexChanged += delegate {
                bool custom = resolutionPreset.SelectedIndex == 4;
                eyeWidth.Enabled = custom;
                eyeHeight.Enabled = custom;
                if (resolutionPreset.SelectedIndex == 0) { eyeWidth.Value = 0; eyeHeight.Value = 0; }
                else if (!custom)
                {
                    string[] values = resolutionPreset.Text.Split('x');
                    eyeWidth.Value = decimal.Parse(values[0].Trim(), CultureInfo.InvariantCulture);
                    eyeHeight.Value = decimal.Parse(values[1].Trim(), CultureInfo.InvariantCulture);
                }
            };
            AddRow(grid, 0, "Per-eye resolution", resolutionPreset, "VR resolution",
                "Select the render resolution for each eye. Higher values improve clarity but require more GPU and transfer bandwidth.");
            AddRow(grid, 1, "Custom width", eyeWidth, "Custom eye width",
                "Width of one VR eye texture in pixels. Used only when Custom is selected.");
            AddRow(grid, 2, "Custom height", eyeHeight, "Custom eye height",
                "Height of one VR eye texture in pixels. Used only when Custom is selected.");

            var fps = NewGroup("VR Framerate", 82);
            panel.Controls.Add(fps);
            var fpsGrid = NewGrid(1);
            fps.Controls.Add(fpsGrid);
            ConfigureCombo(refreshRate, new[] { "Runtime default", "72 FPS", "80 FPS", "90 FPS", "120 FPS" });
            AddRow(fpsGrid, 0, "Refresh rate", refreshRate, "VR framerate",
                "Requests the selected refresh rate from the OpenXR runtime. Runtime default disables the explicit request.");
            return page;
        }

        private TabPage BuildInputTab()
        {
            var page = NewPage("Input");
            var panel = NewSettingsPanel();
            page.Controls.Add(panel);
            var turning = NewGroup("Turning", 120);
            panel.Controls.Add(turning);
            var grid = NewGrid(2);
            turning.Controls.Add(grid);
            AddChoice(grid, 0, "Turn mode", IniTarget.Main, "Input", "CameraModTurnMode", "1",
                new Dictionary<string, string> { { "Snap turn", "0" }, { "Smooth turn", "1" } },
                "Turn mode", "Smooth turn rotates continuously with the right stick. Snap turn rotates by fixed steps.");
            AddNumber(grid, 1, "Smooth-turn speed", IniTarget.Main, "Input", "CameraModSmoothTurnSpeed", "120",
                30, 360, 5, 0, "Smooth-turn speed",
                "Maximum smooth-turn speed in degrees per second. This value is ignored in Snap mode.");

            var damage = NewGroup("Damage Vibration", 160);
            panel.Controls.Add(damage);
            var damageGrid = NewGrid(3);
            damage.Controls.Add(damageGrid);
            AddBoolean(damageGrid, 0, "Vibrate on damage", IniTarget.Main, "MotionControls", "DamageHaptics", "1",
                "Damage vibration", "Vibrates both controllers when Heather's health decreases.");
            AddNumber(damageGrid, 1, "Duration (ms)", IniTarget.Main, "MotionControls", "DamageHapticsDurationMs", "3000",
                10, 10000, 50, 0, "Damage vibration duration", "How long the damage pulse lasts, in milliseconds.");
            AddNumber(damageGrid, 2, "Strength (%)", IniTarget.Main, "MotionControls", "DamageHapticsAmplitudePercent", "100",
                1, 100, 1, 0, "Damage vibration strength", "Controller vibration strength from 1 to 100 percent.");
            return page;
        }

        private TabPage BuildGraphicsTab()
        {
            var page = NewPage("Graphics");
            var panel = NewSettingsPanel();
            page.Controls.Add(panel);
            var aa = NewGroup("Anti-aliasing", 120);
            panel.Controls.Add(aa);
            var aaGrid = NewGrid(2);
            aa.Controls.Add(aaGrid);
            AddChoice(aaGrid, 0, "MSAA", IniTarget.Main, "OpenXR", "MSAASamples", "1",
                new Dictionary<string, string> { { "Off (1x)", "1" }, { "2x", "2" }, { "4x", "4" }, { "8x", "8" } },
                "MSAA", "Multisample anti-aliasing for VR eye textures. Higher levels cost additional GPU performance and memory.");
            AddBoolean(aaGrid, 1, "Enable FXAA", IniTarget.Main, "OpenXR", "EnableFXAA", "1",
                "FXAA", "Fast post-process anti-aliasing. It is inexpensive but can make the image slightly softer.");

            var image = NewGroup("Image and Color Correction", 330);
            panel.Controls.Add(image);
            var imageGrid = NewGrid(9);
            image.Controls.Add(imageGrid);
            AddBoolean(imageGrid, 0, "Game post-process", IniTarget.Main, "Image", "EnableGamePostProcess", "1", "Game post-process", "Keeps Silent Hill 3's original per-eye post-processing enabled.");
            AddDecimal(imageGrid, 1, "Post-process strength", "GamePostProcessStrength", "0.00", -2, 2, 0.05M, 2, "Post-process strength", "Strength adjustment applied to the original game post-process.");
            AddDecimal(imageGrid, 2, "Post-process contrast", "GamePostProcessContrast", "0.88", 0, 3, 0.01M, 2, "Post-process contrast", "Contrast multiplier for the original game post-process.");
            AddDecimal(imageGrid, 3, "Post-process exposure", "GamePostProcessExposureStops", "0.00", -4, 4, 0.05M, 2, "Post-process exposure", "Exposure adjustment for the original game post-process, measured in stops.");
            AddBoolean(imageGrid, 4, "Color correction", IniTarget.Main, "Image", "EnableColorCorrection", "1", "Color correction", "Enables SH3VR exposure, contrast, saturation, and vignette controls.");
            AddDecimal(imageGrid, 5, "Exposure (stops)", "ExposureStops", "1.5", -4, 4, 0.1M, 2, "Exposure", "Overall VR image exposure measured in photographic stops.");
            AddDecimal(imageGrid, 6, "Contrast", "Contrast", "1.0", 0, 3, 0.05M, 2, "Contrast", "Overall VR image contrast multiplier. 1.0 is neutral.");
            AddDecimal(imageGrid, 7, "Saturation", "Saturation", "1.0", 0, 3, 0.05M, 2, "Saturation", "Overall VR color saturation multiplier. 1.0 is neutral.");
            AddDecimal(imageGrid, 8, "Vignette strength", "VignetteStrength", "0.5", 0, 2, 0.05M, 2, "Vignette", "Darkening toward the edges of the VR image. 0 disables the effect.");
            return page;
        }

        private TabPage BuildAdvancedTab()
        {
            var page = NewPage("Advanced");
            var panel = NewSettingsPanel();
            page.Controls.Add(panel);
            var group = NewGroup("Performance", 120);
            panel.Controls.Add(group);
            var grid = NewGrid(2);
            group.Controls.Add(grid);
            AddBoolean(grid, 0, "Automatically enable PC Fix D3D12", IniTarget.Main, "Performance", "AutoEnablePcFixD3D12", "1",
                "PC Fix D3D12 backend", "Automatically enables the faster PC Fix D3D12 path when the compatible D3D9On12 component is available.");
            AddBoolean(grid, 1, "Multicore rendering transfers", IniTarget.Main, "Performance", "MulticoreRendering", "1",
                "Multicore rendering", "Uses two CPU cores for compatible stereo memory transfers. Graphics API calls remain on the render thread.");

            var debug = NewGroup("Debug", 82);
            panel.Controls.Add(debug);
            var debugGrid = NewGrid(1);
            debug.Controls.Add(debugGrid);
            AddBoolean(debugGrid, 0, "Show controller position debug overlay", IniTarget.Main,
                "Debug", "ControllerOrientationOverlay", "0", "Controller debug overlay",
                "Shows controller position and orientation diagnostics after each completed VR eye image. Leave disabled during normal play.");
            return page;
        }

        private TabPage BuildMotionTab()
        {
            var page = NewPage("Motion");
            var tabs = new TabControl { Dock = DockStyle.Fill, Alignment = TabAlignment.Left, Multiline = true };
            page.Controls.Add(tabs);
            tabs.TabPages.Add(BuildMotionGeneralPage());
            tabs.TabPages.Add(BuildLeftHandPage());
            foreach (string weapon in new[] { "Knife", "Steel Pipe", "Maul", "Katana" })
                tabs.TabPages.Add(BuildWeaponPage(weapon, false));
            foreach (string weapon in new[] { "Stun Gun", "Handgun", "Shotgun", "Submachine Gun" })
                tabs.TabPages.Add(BuildWeaponPage(weapon, true));
            return page;
        }

        private TabPage BuildAboutTab()
        {
            var page = NewPage("About");
            var panel = new Panel { Dock = DockStyle.Fill, AutoScroll = true, BackColor = Color.White };
            page.Controls.Add(panel);

            var content = new TableLayoutPanel {
                Dock = DockStyle.Top, AutoSize = true, ColumnCount = 1,
                Padding = new Padding(28, 22, 28, 22), BackColor = Color.White
            };
            content.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100F));
            panel.Controls.Add(content);

            var logo = new PictureBox {
                Dock = DockStyle.Top, Height = 175, SizeMode = PictureBoxSizeMode.Zoom,
                BackColor = Color.White, Margin = new Padding(0, 0, 0, 18)
            };
            using (Stream stream = Assembly.GetExecutingAssembly().GetManifestResourceStream(
                "SH3VRConfig.Assets.SH3VR_LOGO_BLACK.png"))
            {
                if (stream != null)
                {
                    using (Image embedded = Image.FromStream(stream))
                        logo.Image = new Bitmap(embedded);
                }
            }
            content.Controls.Add(logo);

            var description = new Label {
                AutoSize = true, MaximumSize = new Size(820, 0),
                Font = new Font("Segoe UI", 10F), Margin = new Padding(0, 0, 0, 12),
                Text = "Silent Hill 3 VR is an unofficial virtual reality mod for the PC version of Silent Hill 3. " +
                    "It adds stereoscopic 6-DOF VR, motion controls, room-scale movement, tracked weapons, " +
                    "and configurable visual and comfort options."
            };
            content.Controls.Add(description);

            var credit = new Label {
                AutoSize = true, Font = new Font("Segoe UI", 10F, FontStyle.Bold),
                Margin = new Padding(0, 0, 0, 18),
                Text = "Created by NotGodlike with the help of ChatGPT 5.6."
            };
            content.Controls.Add(credit);

            var version = new Label {
                AutoSize = true, Font = new Font("Segoe UI", 10F),
                Margin = new Padding(0, 0, 0, 18),
                Text = "Version: Pre-Release 1.0\r\nRelease date: 09.11.2026"
            };
            content.Controls.Add(version);

            var links = new TableLayoutPanel { AutoSize = true, ColumnCount = 2, Margin = new Padding(0) };
            links.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 180F));
            links.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            content.Controls.Add(links);
            AddAboutLink(links, 0, "Discord", ".notttgoddd", null);
            AddAboutLink(links, 1, "GitHub", "Silent Hill 3 VR Mod",
                "https://github.com/NotGodlikeUwU/Silent-Hill-3-VR-Mod");
            AddAboutLink(links, 2, "Required Camera Mod", "Zealot's Camera Mod",
                "https://github.com/zealottormunds/sh3cammod");
            AddAboutLink(links, 3, "Required PC Fix", "Silent Hill 3 PC Fix by Steam006",
                "https://community.pcgamingwiki.com/files/file/1331-silent-hill-3-pc-fix-by-steam006/");
            return page;
        }

        private void AddAboutLink(TableLayoutPanel grid, int row, string caption,
            string text, string url)
        {
            grid.RowStyles.Add(new RowStyle(SizeType.Absolute, 34F));
            var label = new Label {
                Text = caption, AutoSize = true, Anchor = AnchorStyles.Left,
                Font = new Font(Font, FontStyle.Bold)
            };
            var link = new LinkLabel {
                Text = url == null ? text + "  (click to copy)" : text,
                AutoSize = true, Anchor = AnchorStyles.Left
            };
            link.LinkClicked += delegate {
                if (url == null)
                {
                    Clipboard.SetText(text);
                    statusLabel.Text = "Discord username copied: " + text;
                }
                else
                {
                    try
                    {
                        Process.Start(new ProcessStartInfo(url) { UseShellExecute = true });
                    }
                    catch (Exception error)
                    {
                        MessageBox.Show(this, error.Message, "Unable to open link",
                            MessageBoxButtons.OK, MessageBoxIcon.Error);
                    }
                }
            };
            grid.Controls.Add(label, 0, row);
            grid.Controls.Add(link, 1, row);
        }

        private TabPage BuildMotionGeneralPage()
        {
            var page = NewPage("General");
            var panel = NewSettingsPanel();
            page.Controls.Add(panel);
            var group = NewGroup("General Motion Settings", 82);
            panel.Controls.Add(group);
            var grid = NewGrid(1);
            group.Controls.Add(grid);
            AddBoolean(grid, 0, "Vibrate right controller when firing", IniTarget.Main,
                "MotionControls", "FirearmHaptics", "1", "Firearm vibration",
                "Applies a medium recoil pulse to the right controller for each confirmed firearm discharge.");
            return page;
        }

        private TabPage BuildLeftHandPage()
        {
            var page = NewPage("Left Hand");
            var panel = NewSettingsPanel();
            page.Controls.Add(panel);
            var group = NewGroup("Left Hand", 370);
            panel.Controls.Add(group);
            var grid = NewGrid(10);
            group.Controls.Add(grid);
            AddBoolean(grid, 0, "Enabled", IniTarget.Weapons, "LeftHand", "Enabled", "1", "Left hand", "Shows the tracked left-hand model.");
            AddBoolean(grid, 1, "Adaptive lighting", IniTarget.Weapons, "LeftHand", "AdaptiveLighting", "1", "Adaptive lighting", "Makes the hand react to scene and flashlight lighting.");
            AddNumber(grid, 2, "Lighting updates (Hz)", IniTarget.Weapons, "LeftHand", "LightingUpdateHz", "20", 1, 60, 1, 0, "Lighting update rate", "Number of environmental-light samples per second.");
            AddMotionTransformRows(grid, "LeftHand", 3, "100", true);
            return page;
        }

        private TabPage BuildWeaponPage(string weapon, bool firearm)
        {
            var page = NewPage(weapon);
            var panel = NewSettingsPanel();
            page.Controls.Add(panel);
            var group = NewGroup(weapon + " calibration", firearm ? 475 : 315);
            panel.Controls.Add(group);
            var grid = NewGrid(firearm ? 13 : 8);
            group.Controls.Add(grid);
            int row = 0;
            if (firearm)
            {
                AddBoolean(grid, row++, "Muzzle flash", IniTarget.Weapons, weapon, "ShowMuzzleFlash", "1", "Muzzle flash", "Shows a tracked muzzle-flash effect when this weapon fires.");
                AddNumber(grid, row++, "Flash size (%)", IniTarget.Weapons, weapon, "MuzzleFlashSizePercent", "100", 1, 400, 1, 0, "Muzzle-flash size", "Size of this weapon's muzzle-flash effect.");
                AddBoolean(grid, row++, "Muzzle smoke", IniTarget.Weapons, weapon, "ShowMuzzleSmoke", weapon == "Stun Gun" ? "0" : "1", "Muzzle smoke", "Shows a tracked smoke effect after this weapon fires.");
                AddNumber(grid, row++, "Smoke size (%)", IniTarget.Weapons, weapon, "MuzzleSmokeSizePercent", "100", 1, 400, 1, 0, "Muzzle-smoke size", "Size of this weapon's muzzle-smoke effect.");
            }
            else
                AddBoolean(grid, row++, "Show guide dot", IniTarget.Weapons, weapon, "ShowGuideDot", "1", "Guide dot", "Shows the in-world alignment guide for this weapon.");
            AddMotionTransformRows(grid, weapon, row, weapon == "Knife" ? "70" : "100", false);
            row += 7;
            if (firearm)
            {
                AddNumber(grid, row++, "Aim pitch (degrees)", IniTarget.Weapons, weapon, "AimPitchDegrees", "-18", -360, 360, 1, 0, "Aim pitch", "Vertical correction shared by the bullet ray and reticle.");
                AddNumber(grid, row, "Aim yaw (degrees)", IniTarget.Weapons, weapon, "AimYawDegrees", "0", -360, 360, 1, 0, "Aim yaw", "Horizontal correction shared by the bullet ray and reticle.");
            }
            return page;
        }

        private void AddMotionTransformRows(TableLayoutPanel grid, string section, int row, string scaleDefault, bool leftHand)
        {
            AddNumber(grid, row++, "Scale (%)", IniTarget.Weapons, section, "ScalePercent", scaleDefault, 1, 400, 1, 0, "Model scale", "Tracked model size as a percentage of its original scale.");
            AddNumber(grid, row++, "Position X", IniTarget.Weapons, section, "PositionX", "0", -2000, 2000, 1, 0, "Position X", "Controller-local horizontal offset: positive is right, negative is left.");
            AddNumber(grid, row++, "Position Y", IniTarget.Weapons, section, "PositionY", "0", -2000, 2000, 1, 0, "Position Y", "Controller-local vertical offset: positive is down, negative is up.");
            AddNumber(grid, row++, "Position Z", IniTarget.Weapons, section, "PositionZ", "0", -2000, 2000, 1, 0, "Position Z", "Controller-local depth offset: positive is forward, negative is backward.");
            AddNumber(grid, row++, "Pitch (degrees)", IniTarget.Weapons, section, "RotationPitchDegrees", leftHand ? "0" : "90", -720, 720, 1, 0, "Pitch rotation", "Rotation around the controller-local pitch axis.");
            AddNumber(grid, row++, "Yaw (degrees)", IniTarget.Weapons, section, "RotationYawDegrees", leftHand ? "0" : "90", -720, 720, 1, 0, "Yaw rotation", "Rotation around the controller-local yaw axis.");
            AddNumber(grid, row, "Roll (degrees)", IniTarget.Weapons, section, "RotationRollDegrees", leftHand ? "0" : "45", -720, 720, 1, 0, "Roll rotation", "Rotation around the controller-local roll axis.");
        }

        private void LoadSettings()
        {
            configurationFilesAvailable = File.Exists(mainIniPath) && File.Exists(weaponsIniPath);
            if (!configurationFilesAvailable)
                MessageBox.Show(this, "Place SH3VRConfig.exe next to sh3.exe, sh3vr.ini, and sh3vr_weapons.ini.", "Configuration files not found", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            mainIni = IniDocument.Load(mainIniPath);
            weaponsIni = IniDocument.Load(weaponsIniPath);
            foreach (SettingBinding binding in bindings)
                SetControlValue(binding, GetDocument(binding.Target).Get(binding.Section, binding.Key, binding.DefaultValue));
            LoadResolutionAndRefresh();
            statusLabel.Text = "Editing: " + mainIniPath;
            descriptionTitle.Text = "SH3VR Configuration";
            descriptionText.Text = "Move the pointer over a setting to see what it changes.";
        }

        private void LoadResolutionAndRefresh()
        {
            int width = ParseInt(mainIni.Get("OpenXR", "EyeWidth", "1900"), 1900);
            int height = ParseInt(mainIni.Get("OpenXR", "EyeHeight", "2076"), 2076);
            eyeWidth.Value = Clamp(width, eyeWidth.Minimum, eyeWidth.Maximum);
            eyeHeight.Value = Clamp(height, eyeHeight.Minimum, eyeHeight.Maximum);
            string preset = width + " x " + height;
            if (width == 0 && height == 0) resolutionPreset.SelectedIndex = 0;
            else if (resolutionPreset.Items.Contains(preset)) resolutionPreset.SelectedItem = preset;
            else resolutionPreset.SelectedIndex = 4;
            int request = ParseInt(mainIni.Get("OpenXR", "RequestRefreshRate", "1"), 1);
            int fps = ParseInt(mainIni.Get("OpenXR", "TargetRefreshRate", "90"), 90);
            refreshRate.SelectedItem = request == 0 || fps == 0 ? "Runtime default" : fps + " FPS";
            if (refreshRate.SelectedIndex < 0) refreshRate.SelectedItem = "90 FPS";
        }

        private bool SaveSettings()
        {
            if (!configurationFilesAvailable)
            {
                MessageBox.Show(this, "Configuration files are missing. No files were written.", "Unable to save", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return false;
            }
            try
            {
                foreach (SettingBinding binding in bindings)
                    GetDocument(binding.Target).Set(binding.Section, binding.Key, GetControlValue(binding));
                mainIni.Set("OpenXR", "EyeWidth", ((int)eyeWidth.Value).ToString(CultureInfo.InvariantCulture));
                mainIni.Set("OpenXR", "EyeHeight", ((int)eyeHeight.Value).ToString(CultureInfo.InvariantCulture));
                bool runtimeDefault = refreshRate.Text == "Runtime default";
                int fps = runtimeDefault ? 0 : ParseInt(refreshRate.Text.Replace(" FPS", string.Empty), 90);
                mainIni.Set("OpenXR", "TargetRefreshRate", fps.ToString(CultureInfo.InvariantCulture));
                mainIni.Set("OpenXR", "RequestRefreshRate", runtimeDefault ? "0" : "1");
                mainIni.Save(mainIniPath);
                weaponsIni.Save(weaponsIniPath);
                statusLabel.Text = "Saved successfully. Backups: sh3vr.ini.bak and sh3vr_weapons.ini.bak";
                return true;
            }
            catch (Exception error)
            {
                MessageBox.Show(this, error.Message, "Unable to save", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return false;
            }
        }

        private void SaveAndLaunch()
        {
            if (!SaveSettings()) return;
            string game = Path.Combine(gameDirectory, "sh3.exe");
            if (!File.Exists(game))
            {
                MessageBox.Show(this, "sh3.exe was not found next to the configuration tool.", "Game not found", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            Process.Start(new ProcessStartInfo(game) { WorkingDirectory = gameDirectory, UseShellExecute = true });
            Close();
        }

        private void LoadDefaults()
        {
            foreach (SettingBinding binding in bindings) SetControlValue(binding, binding.DefaultValue);
            resolutionPreset.SelectedItem = "1900 x 2076";
            refreshRate.SelectedItem = "90 FPS";
            statusLabel.Text = "Recommended defaults loaded. Click Save to apply them.";
        }

        private IniDocument GetDocument(IniTarget target) { return target == IniTarget.Main ? mainIni : weaponsIni; }

        private void AddDecimal(TableLayoutPanel grid, int row, string label, string key, string fallback,
            decimal min, decimal max, decimal increment, int decimals, string title, string description)
        {
            AddNumber(grid, row, label, IniTarget.Main, "Image", key, fallback, min, max, increment, decimals, title, description);
        }

        private void AddBoolean(TableLayoutPanel grid, int row, string label, IniTarget target,
            string section, string key, string fallback, string title, string description)
        {
            var check = new CheckBox { AutoSize = true, Text = label, Anchor = AnchorStyles.Left };
            grid.Controls.Add(check, 0, row);
            grid.SetColumnSpan(check, 2);
            Register(target, section, key, fallback, ValueKind.Boolean, check, null, title, description);
        }

        private void AddNumber(TableLayoutPanel grid, int row, string label, IniTarget target,
            string section, string key, string fallback, decimal min, decimal max, decimal increment,
            int decimals, string title, string description)
        {
            var number = new NumericUpDown();
            ConfigureNumber(number, min, max, increment, decimals);
            AddRow(grid, row, label, number, title, description);
            Register(target, section, key, fallback, decimals == 0 ? ValueKind.Integer : ValueKind.Decimal, number, null, title, description);
        }

        private void AddChoice(TableLayoutPanel grid, int row, string label, IniTarget target,
            string section, string key, string fallback, Dictionary<string, string> choices,
            string title, string description)
        {
            var combo = new ComboBox();
            ConfigureCombo(combo, new List<string>(choices.Keys).ToArray());
            AddRow(grid, row, label, combo, title, description);
            Register(target, section, key, fallback, ValueKind.Choice, combo, choices, title, description);
        }

        private void Register(IniTarget target, string section, string key, string fallback,
            ValueKind kind, Control control, Dictionary<string, string> choices, string title, string description)
        {
            bindings.Add(new SettingBinding { Target = target, Section = section, Key = key,
                DefaultValue = fallback, Kind = kind, Control = control, Choices = choices });
            AttachDescription(control, title, description);
        }

        private void SetControlValue(SettingBinding binding, string value)
        {
            if (binding.Kind == ValueKind.Boolean)
                ((CheckBox)binding.Control).Checked = ParseInt(value, ParseInt(binding.DefaultValue, 0)) != 0;
            else if (binding.Kind == ValueKind.Choice)
            {
                var combo = (ComboBox)binding.Control;
                foreach (KeyValuePair<string, string> choice in binding.Choices)
                    if (string.Equals(choice.Value, value, StringComparison.OrdinalIgnoreCase)) combo.SelectedItem = choice.Key;
                if (combo.SelectedIndex < 0)
                    foreach (KeyValuePair<string, string> choice in binding.Choices)
                        if (choice.Value == binding.DefaultValue) combo.SelectedItem = choice.Key;
            }
            else
            {
                var number = (NumericUpDown)binding.Control;
                decimal parsed;
                if (!decimal.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out parsed))
                    decimal.TryParse(binding.DefaultValue, NumberStyles.Float, CultureInfo.InvariantCulture, out parsed);
                number.Value = Clamp(parsed, number.Minimum, number.Maximum);
            }
        }

        private string GetControlValue(SettingBinding binding)
        {
            if (binding.Kind == ValueKind.Boolean) return ((CheckBox)binding.Control).Checked ? "1" : "0";
            if (binding.Kind == ValueKind.Choice)
            {
                string text = ((ComboBox)binding.Control).Text;
                return binding.Choices.ContainsKey(text) ? binding.Choices[text] : binding.DefaultValue;
            }
            return ((NumericUpDown)binding.Control).Value.ToString(binding.Kind == ValueKind.Integer ? "0" : "0.##", CultureInfo.InvariantCulture);
        }

        private void AttachDescription(Control control, string title, string description)
        {
            EventHandler show = delegate { descriptionTitle.Text = title; descriptionText.Text = description; };
            control.MouseEnter += show;
            control.Enter += show;
        }

        private static TabPage NewPage(string title) { return new TabPage(title) { Padding = new Padding(8) }; }
        private static FlowLayoutPanel NewSettingsPanel() { return new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.TopDown, AutoScroll = true, WrapContents = false, Padding = new Padding(4) }; }
        private static GroupBox NewGroup(string title, int height) { return new GroupBox { Text = title, Width = 900, Height = height, Padding = new Padding(12, 18, 12, 8) }; }
        private static TableLayoutPanel NewGrid(int rows)
        {
            var grid = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, RowCount = rows };
            grid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 52));
            grid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 48));
            for (int i = 0; i < rows; ++i) grid.RowStyles.Add(new RowStyle(SizeType.Absolute, 32));
            return grid;
        }

        private void AddRow(TableLayoutPanel grid, int row, string labelText, Control control, string title, string description)
        {
            var label = new Label { Text = labelText, AutoSize = true, Anchor = AnchorStyles.Left };
            control.Anchor = AnchorStyles.Left | AnchorStyles.Right;
            control.Margin = new Padding(3, 4, 20, 4);
            grid.Controls.Add(label, 0, row);
            grid.Controls.Add(control, 1, row);
            AttachDescription(label, title, description);
            AttachDescription(control, title, description);
        }

        private static void ConfigureCombo(ComboBox combo, string[] values)
        {
            combo.DropDownStyle = ComboBoxStyle.DropDownList;
            combo.Items.AddRange(values);
            if (combo.Items.Count > 0) combo.SelectedIndex = 0;
        }

        private static void ConfigureNumber(NumericUpDown number, decimal min, decimal max, decimal increment, int decimals)
        {
            number.Minimum = min; number.Maximum = max; number.Increment = increment;
            number.DecimalPlaces = decimals; number.ThousandsSeparator = true;
        }

        private static Button MakeButton(string text, int width, EventHandler click)
        {
            var button = new Button { Text = text, Width = width, Height = 30, Margin = new Padding(4) };
            button.Click += click;
            return button;
        }

        private static int ParseInt(string text, int fallback)
        {
            int value;
            return int.TryParse(text, NumberStyles.Integer, CultureInfo.InvariantCulture, out value) ? value : fallback;
        }

        private static decimal Clamp(decimal value, decimal min, decimal max)
        {
            return Math.Max(min, Math.Min(max, value));
        }
    }
}
