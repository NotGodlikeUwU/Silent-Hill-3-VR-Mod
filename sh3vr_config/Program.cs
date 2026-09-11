using System;
using System.IO;
using System.Windows.Forms;

namespace SH3VRConfig
{
    internal static class Program
    {
        [STAThread]
        private static int Main(string[] args)
        {
            if (args.Length > 0 && string.Equals(args[0], "--validate", StringComparison.OrdinalIgnoreCase))
            {
                string directory = args.Length > 1 ? args[1] : AppDomain.CurrentDomain.BaseDirectory;
                string main = Path.Combine(directory, "sh3vr.ini");
                string weapons = Path.Combine(directory, "sh3vr_weapons.ini");
                if (!File.Exists(main) || !File.Exists(weapons)) return 2;
                IniDocument.Load(main); IniDocument.Load(weapons);
                return 0;
            }
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new MainForm());
            return 0;
        }
    }
}
