/*****************************************************************************
 * Copyright (c) 2014-2020 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "TitleSequenceManager.h"

#include "../Context.h"
#include "../OpenRCT2.h"
#include "../PlatformEnvironment.h"
#include "../core/Collections.hpp"
#include "../core/FileScanner.h"
#include "../core/Memory.hpp"
#include "../core/Path.hpp"
#include "../core/String.hpp"
#include "../localisation/Localisation.h"
#include "../platform/platform.h"
#include "TitleSequence.h"

#include <algorithm>
#include <iterator>
#include <vector>

namespace TitleSequenceManager
{
    struct PredefinedSequence
    {
        const utf8* ConfigId;
        const utf8* Filename;
        rct_string_id StringId;
    };

    static constexpr PredefinedSequence PredefinedSequences[] = {
        { "*RCT1", "rct1.parkseq", STR_TITLE_SEQUENCE_RCT1 },
        { "*RCT1AA", "rct1aa.parkseq", STR_TITLE_SEQUENCE_RCT1_AA },
        { "*RCT1AALL", "rct1aall.parkseq", STR_TITLE_SEQUENCE_RCT1_AA_LL },
        { "*RCT2", "rct2.parkseq", STR_TITLE_SEQUENCE_RCT2 },
        { "*OPENRCT2", "openrct2.parkseq", STR_TITLE_SEQUENCE_OPENRCT2 },
    };

    static std::vector<TitleSequenceManagerItem> _items;

    static std::string GetNewTitleSequencePath(const std::string& name, bool isZip);
    static size_t FindItemIndexByPath(const utf8* path);
    static void Scan(const utf8* directory);
    static void AddSequence(const utf8* scanPath);
    static void SortSequences();
    static std::string GetNameFromSequencePath(const std::string& path);
    static std::string GetDataSequencesPath();
    static std::string GetUserSequencesPath();
    static bool IsNameReserved(const std::string& name);

    size_t GetCount()
    {
        return _items.size();
    }

    const TitleSequenceManagerItem* GetItem(size_t i)
    {
        if (i >= _items.size())
        {
            return nullptr;
        }
        return &_items[i];
    }

    static size_t FindItemIndexByPath(const utf8* path)
    {
        size_t index = Collections::IndexOf(
            _items, [path](const TitleSequenceManagerItem& item) -> bool { return String::Equals(path, item.Path.c_str()); });
        return index;
    }

    void DeleteItem(size_t i)
    {
        auto item = GetItem(i);
        if (item == nullptr)
        {
            return;
        }
        const utf8* path = item->Path.c_str();
        if (item->IsZip)
        {
            platform_file_delete(path);
        }
        else
        {
            platform_directory_delete(path);
        }
        _items.erase(_items.begin() + i);
    }

    size_t RenameItem(size_t i, const utf8* newName)
    {
        auto item = &_items[i];
        const auto& oldPath = item->Path;

        auto newPath = Path::Combine(Path::GetDirectory(oldPath), newName);
        if (item->IsZip)
        {
            newPath += TITLE_SEQUENCE_EXTENSION;
            platform_file_move(oldPath.c_str(), newPath.c_str());
        }
        else
        {
            platform_file_move(oldPath.c_str(), newPath.c_str());
        }

        item->Name = std::string(newName);
        item->Path = std::string(newPath);

        SortSequences();
        size_t index = FindItemIndexByPath(newPath.c_str());
        return index;
    }

    size_t DuplicateItem(size_t i, const utf8* name)
    {
        auto item = &_items[i];
        const auto& srcPath = item->Path;

        std::string dstPath = GetNewTitleSequencePath(std::string(name), item->IsZip);
        if (!platform_file_copy(srcPath.c_str(), dstPath.c_str(), true))
        {
            return SIZE_MAX;
        }

        AddSequence(dstPath.c_str());
        SortSequences();
        size_t index = FindItemIndexByPath(dstPath.c_str());
        return index;
    }

    size_t CreateItem(const utf8* name)
    {
        auto seq = CreateTitleSequence();
        seq->Name = name;
        seq->Path = GetNewTitleSequencePath(std::string(name), true);
        seq->IsZip = true;

        bool success = TitleSequenceSave(*seq);

        size_t index = SIZE_MAX;
        if (success)
        {
            AddSequence(seq->Path.c_str());
            SortSequences();
            index = FindItemIndexByPath(seq->Path.c_str());
        }
        return index;
    }

    static std::string GetNewTitleSequencePath(const std::string& name, bool isZip)
    {
        auto path = Path::Combine(GetUserSequencesPath(), name);
        if (isZip)
        {
            path += TITLE_SEQUENCE_EXTENSION;
        }
        return path;
    }

    static size_t GetPredefinedIndex(const std::string& path)
    {
        const utf8* filename = Path::GetFileName(path.c_str());
        for (size_t i = 0; i < std::size(PredefinedSequences); i++)
        {
            if (String::Equals(filename, PredefinedSequences[i].Filename, true))
            {
                return i;
            }
        }
        return PREDEFINED_INDEX_CUSTOM;
    }

    static void SortSequences()
    {
        // Sort sequences by predefined index and then name
        std::sort(
            _items.begin(), _items.end(), [](const TitleSequenceManagerItem& a, const TitleSequenceManagerItem& b) -> bool {
                if (a.PredefinedIndex < b.PredefinedIndex)
                {
                    return true;
                }
                else if (a.PredefinedIndex > b.PredefinedIndex)
                {
                    return false;
                }
                return _strcmpi(a.Name.c_str(), b.Name.c_str()) < 0;
            });
    }

    void Scan()
    {
        _items.clear();

        // Scan data path
        auto path = GetDataSequencesPath();
        Scan(path.c_str());

        // Scan user path
        path = GetUserSequencesPath();
        Scan(path.c_str());

        SortSequences();
    }

    static void Scan(const utf8* directory)
    {
        utf8 pattern[MAX_PATH];
        String::Set(pattern, sizeof(pattern), directory);
        Path::Append(pattern, sizeof(pattern), "script.txt;*.parkseq");

        IFileScanner* fileScanner = Path::ScanDirectory(pattern, true);
        while (fileScanner->Next())
        {
            const utf8* path = fileScanner->GetPath();
            AddSequence(path);
        }
        delete fileScanner;
    }

    static void AddSequence(const utf8* scanPath)
    {
        TitleSequenceManagerItem item;

        std::string path;
        bool isZip = true;
        if (String::Equals(Path::GetExtension(scanPath), ".txt", true))
        {
            // If we are given a .txt file, set the path to the containing directory
            path = Path::GetDirectory(std::string(scanPath));
            isZip = false;
            item.Name = Path::GetFileName(path);
        }
        else
        {
            path = std::string(scanPath);
            item.Name = GetNameFromSequencePath(path);
        }

        item.PredefinedIndex = GetPredefinedIndex(path);
        item.Path = path;
        if (item.PredefinedIndex != PREDEFINED_INDEX_CUSTOM)
        {
            rct_string_id stringId = PredefinedSequences[item.PredefinedIndex].StringId;
            item.Name = language_get_string(stringId);
        }
        else if (IsNameReserved(item.Name))
        {
            // Reserved names are not allowed because they map to the
            // actual predefined names and also prevent editing
            return;
        }
        item.IsZip = isZip;
        _items.push_back(item);
    }

    static std::string GetNameFromSequencePath(const std::string& path)
    {
        auto name = Path::GetFileNameWithoutExtension(path);
        return name;
    }

    static std::string GetDataSequencesPath()
    {
        auto env = OpenRCT2::GetContext()->GetPlatformEnvironment();
        return env->GetDirectoryPath(OpenRCT2::DIRBASE::OPENRCT2, OpenRCT2::DIRID::SEQUENCE);
    }

    static std::string GetUserSequencesPath()
    {
        auto env = OpenRCT2::GetContext()->GetPlatformEnvironment();
        return env->GetDirectoryPath(OpenRCT2::DIRBASE::USER, OpenRCT2::DIRID::SEQUENCE);
    }

    static bool IsNameReserved(const std::string& name)
    {
        for (const auto& pseq : TitleSequenceManager::PredefinedSequences)
        {
            auto predefinedName = Path::GetFileNameWithoutExtension(std::string(pseq.Filename));
            if (String::Equals(name, predefinedName, true))
            {
                return true;
            }
        }
        return false;
    }
} // namespace TitleSequenceManager

size_t title_sequence_manager_get_count()
{
    return TitleSequenceManager::GetCount();
}

const utf8* title_sequence_manager_get_name(size_t index)
{
    auto item = TitleSequenceManager::GetItem(index);
    if (item == nullptr)
    {
        return nullptr;
    }
    return item->Name.c_str();
}

const utf8* title_sequence_manager_get_path(size_t index)
{
    auto item = TitleSequenceManager::GetItem(index);
    if (item == nullptr)
    {
        return nullptr;
    }
    return item->Path.c_str();
}

const utf8* title_sequence_manager_get_config_id(size_t index)
{
    auto item = TitleSequenceManager::GetItem(index);
    if (item == nullptr)
    {
        return nullptr;
    }
    const auto& name = item->Name;
    auto filename = Path::GetFileName(name);
    for (const auto& pseq : TitleSequenceManager::PredefinedSequences)
    {
        if (String::Equals(filename, pseq.Filename, true))
        {
            return pseq.ConfigId;
        }
    }
    return name.c_str();
}

size_t title_sequence_manager_get_predefined_index(size_t index)
{
    auto item = TitleSequenceManager::GetItem(index);
    if (item == nullptr)
    {
        return 0;
    }
    size_t predefinedIndex = item->PredefinedIndex;
    return predefinedIndex;
}

size_t title_sequence_manager_get_index_for_config_id(const utf8* configId)
{
    size_t count = TitleSequenceManager::GetCount();
    for (size_t i = 0; i < count; i++)
    {
        const utf8* cid = title_sequence_manager_get_config_id(i);
        if (String::Equals(cid, configId))
        {
            return i;
        }
    }
    return SIZE_MAX;
}

size_t title_sequence_manager_get_index_for_name(const utf8* name)
{
    size_t count = TitleSequenceManager::GetCount();
    for (size_t i = 0; i < count; i++)
    {
        const utf8* tn = title_sequence_manager_get_name(i);
        if (String::Equals(tn, name))
        {
            return i;
        }
    }
    return SIZE_MAX;
}

bool title_sequence_manager_is_name_reserved(const utf8* name)
{
    return TitleSequenceManager::IsNameReserved(name);
}

void title_sequence_manager_scan()
{
    TitleSequenceManager::Scan();
}

void title_sequence_manager_delete(size_t i)
{
    TitleSequenceManager::DeleteItem(i);
}

size_t title_sequence_manager_rename(size_t i, const utf8* name)
{
    return TitleSequenceManager::RenameItem(i, name);
}

size_t title_sequence_manager_duplicate(size_t i, const utf8* name)
{
    return TitleSequenceManager::DuplicateItem(i, name);
}

size_t title_sequence_manager_create(const utf8* name)
{
    return TitleSequenceManager::CreateItem(name);
}
