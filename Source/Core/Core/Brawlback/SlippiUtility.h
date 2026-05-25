#pragma once

#include "Common/CommonTypes.h"
#include <unordered_map>
#include "Common/ChunkFile.h"

/*
============================================
This, along with SlippiUtility.cpp is a place to hold code from the Slippi repo.

There may be things that come from Slippi in other places as well,
but this is meant to be a centralized place where large chunks of code taken from Slippi can live.

If the Slippi team feels uncomfortable with their code being used in this project,
having this code in this one file makes it easier to replace it with our own logic if the need should arise.


https://github.com/project-slippi/Ishiiruka/



A very heartfelt thank you to the Slippi team for all their hard work and dedication. <3
============================================
*/

namespace SlippiUtility
{

    namespace Savestate
    {

    // Types
    struct PreserveBlock
    {
        u32 address;
        u32 length;

        bool operator==(const PreserveBlock& p) const
        {
        return address == p.address && length == p.length;
        }
    };

    typedef struct
    {
        u32 startAddress;
        u32 endAddress;
        u8* data;
        std::string regionName;
    } ssBackupLoc;

    struct preserve_hash_fn
    {
        std::size_t operator()(const PreserveBlock& node) const
        {
        return node.address ^ node.length;  // TODO: This is probably a bad hash
        }
    };

    typedef struct
    {
        u32 address;
        u32 value;
    } ssBackupStaticToHeapPtr;


    // Funcs
    void SlippiInitBackupLocations(std::vector<ssBackupLoc>& backupLocs,
                                    std::vector<ssBackupLoc>& fullBackupRegions,
                                    std::vector<PreserveBlock>& excludeSections);

    } // namespace Savestate


    namespace Mem
    {
        std::vector<u8> uint16ToVector(u16 num);
        std::vector<u8> uint32ToVector(u32 num);
        std::vector<u8> int32ToVector(int32_t num);
        void appendWordToBuffer(std::vector<u8>* buf, u32 word);
        void appendHalfToBuffer(std::vector<u8>* buf, u16 word);
        uint8_t readByte(uint8_t* a, int& idx, uint32_t maxSize, uint8_t defaultValue);
        uint16_t readHalf(uint8_t* a, int& idx, uint32_t maxSize, uint16_t defaultValue);
        uint32_t readWord(uint8_t* a, int& idx, uint32_t maxSize, uint32_t defaultValue);
        uint32_t readWord(uint8_t* a);
        float readFloat(uint8_t* a, int& idx, uint32_t maxSize, float defaultValue);
    }  // namespace Mem

}
