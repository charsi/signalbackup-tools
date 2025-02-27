/*
  Copyright (C) 2022-2023  Selwin van Dijk

  This file is part of signalbackup-tools.

  signalbackup-tools is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  signalbackup-tools is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with signalbackup-tools.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "signalbackup.ih"

std::pair<std::string, std::string> SignalBackup::getDesktopDir() const
{
#if defined(_WIN32) || defined(__MINGW64__)

  // Windows: concatenate HOMEDRIVE+HOMEPATH
  // probably only works on windows 7 and newer? (if at all)
  const char *homedrive_cs = getenv("HOMEDRIVE");
  const char *homepath_cs = getenv("HOMEPATH");
  if (homedrive_cs == nullptr || homepath_cs == nullptr)
    return {std::string(), std::string()};
  std::string home = std::string(homedrive_cs) + std::string(homepath_cs);
  if (home.empty())
    return {std::string(), std::string()};

  if (bepaald::isDir(home + "/AppData/Roaming/Signal"))
    return {home + "/AppData/Roaming/Signal", home + "/AppData/Roaming/Signal"};
  else if (bepaald::isDir(home + "/AppData/Roaming/Signal Beta"))
    return {home + "/AppData/Roaming/Signal Beta", home + "/AppData/Roaming/Signal Beta"};
  else
    return {std::string(), std::string()};
#else
  char const *homedir_cs = getenv("HOME");
  if (homedir_cs == nullptr)
    return {std::string(), std::string()};
  std::string homedir(homedir_cs);
  if (homedir.empty())
    return {std::string(), std::string()};
#if defined(__APPLE__) && defined(__MACH__)
  if (bepaald::isDir(homedir + "/Library/Application Support/Signal"))
    return {homedir + "/Library/Application Support/Signal", homedir + "/Library/Application Support/Signal"};
  if (bepaald::isDir(homedir + "/Library/Application Support/Signal Beta"))
    return {homedir + "/Library/Application Support/Signal Beta", homedir + "/Library/Application Support/Signal Beta"};
  else
    return {std::string(), std::string()};
#else // !windows && !mac
  if (bepaald::isDir(homedir + "/.config/Signal"))
    return {homedir + "/.config/Signal", homedir + "/.config/Signal"};
  if (bepaald::isDir(homedir + "/.config/Signal Beta"))
    return {homedir + "/.config/Signal Beta", homedir + "/.config/Signal Beta"};
  else
    return {std::string(), std::string()};
#endif
#endif
}
