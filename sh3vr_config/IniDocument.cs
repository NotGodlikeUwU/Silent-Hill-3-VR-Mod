using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace SH3VRConfig
{
    internal sealed class IniDocument
    {
        private readonly List<string> lines = new List<string>();

        public static IniDocument Load(string path)
        {
            var document = new IniDocument();
            if (File.Exists(path)) document.lines.AddRange(File.ReadAllLines(path));
            return document;
        }

        public string Get(string section, string key, string fallback)
        {
            bool inSection = false;
            foreach (string raw in lines)
            {
                string line = raw.Trim();
                if (line.StartsWith("[") && line.EndsWith("]"))
                {
                    inSection = string.Equals(line.Substring(1, line.Length - 2).Trim(), section,
                        StringComparison.OrdinalIgnoreCase);
                    continue;
                }
                if (!inSection || line.Length == 0 || line[0] == ';' || line[0] == '#') continue;
                int equals = line.IndexOf('=');
                if (equals >= 0 && string.Equals(line.Substring(0, equals).Trim(), key,
                    StringComparison.OrdinalIgnoreCase))
                    return line.Substring(equals + 1).Trim();
            }
            return fallback;
        }

        public void Set(string section, string key, string value)
        {
            int sectionStart = -1;
            int sectionEnd = lines.Count;
            for (int i = 0; i < lines.Count; ++i)
            {
                string line = lines[i].Trim();
                if (!line.StartsWith("[") || !line.EndsWith("]")) continue;
                string current = line.Substring(1, line.Length - 2).Trim();
                if (sectionStart >= 0) { sectionEnd = i; break; }
                if (string.Equals(current, section, StringComparison.OrdinalIgnoreCase)) sectionStart = i;
            }
            if (sectionStart < 0)
            {
                if (lines.Count > 0 && lines[lines.Count - 1].Length != 0) lines.Add(string.Empty);
                lines.Add("[" + section + "]");
                lines.Add(key + "=" + value);
                return;
            }
            for (int i = sectionStart + 1; i < sectionEnd; ++i)
            {
                string line = lines[i].Trim();
                if (line.Length == 0 || line[0] == ';' || line[0] == '#') continue;
                int equals = line.IndexOf('=');
                if (equals >= 0 && string.Equals(line.Substring(0, equals).Trim(), key,
                    StringComparison.OrdinalIgnoreCase))
                {
                    lines[i] = key + "=" + value;
                    return;
                }
            }
            lines.Insert(sectionEnd, key + "=" + value);
        }

        public void Save(string path)
        {
            string temporary = path + ".tmp";
            File.WriteAllLines(temporary, lines.ToArray(), new UTF8Encoding(false));
            if (File.Exists(path)) File.Replace(temporary, path, path + ".bak", true);
            else File.Move(temporary, path);
        }
    }
}
