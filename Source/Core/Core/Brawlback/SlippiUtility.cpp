#include "SlippiUtility.h"


namespace SlippiUtility
{


    namespace Savestate
    {

    bool cmpFn(PreserveBlock pb1, PreserveBlock pb2)
    {
        return pb1.address < pb2.address;
    }

    void SlippiInitBackupLocations(std::vector<ssBackupLoc>& backupLocs,
                                    std::vector<ssBackupLoc>& fullBackupRegions,
                                    std::vector<PreserveBlock>& excludeSections)
    {
        static std::vector<ssBackupLoc> processedLocs = {};

        // If the processed locations are already computed, just copy them directly
        if (processedLocs.size() /*&& !shouldForceInit*/)
        {
            backupLocs.insert(backupLocs.end(), processedLocs.begin(), processedLocs.end());
            return;
        }

        // shouldForceInit = false;

        // Get Main Heap Boundaries
        // fullBackupRegions[3].startAddress = PowerPC::HostRead_U32(0x804d76b8); // <- from melee
        // fullBackupRegions[3].endAddress = PowerPC::HostRead_U32(0x804d76bc);   // <- from melee
        // WARN_LOG(BRAWLBACK, "Heap start is: 0x%X", fullBackupRegions[3].startAddress);
        // WARN_LOG(BRAWLBACK, "Heap end is: 0x%X", fullBackupRegions[3].endAddress);

        // Sort exclude sections
        std::sort(excludeSections.begin(), excludeSections.end(), cmpFn);

        // Initialize backupLocs to full regions
        backupLocs.insert(backupLocs.end(), fullBackupRegions.begin(), fullBackupRegions.end());

        // Remove exclude sections from backupLocs
        int idx = 0;
        for (auto it = excludeSections.begin(); it != excludeSections.end(); ++it)
        {
            PreserveBlock ipb = *it;

            while (ipb.length > 0)
            {
                // Move up the backupLocs index until we reach a section relevant to us
                while (idx < backupLocs.size() && ipb.address >= backupLocs[idx].endAddress)
                {
                    idx += 1;
                }

                // Once idx is beyond backup locs, we are already not backup up this exclusion section
                if (idx >= backupLocs.size())
                {
                    break;
                }

                // Handle case where our exclusion starts before the actual backup section
                if (ipb.address < backupLocs[idx].startAddress)
                {
                    int newSize = (s32)ipb.length - ((s32)backupLocs[idx].startAddress - (s32)ipb.address);

                    ipb.length = newSize > 0 ? newSize : 0;
                    ipb.address = backupLocs[idx].startAddress;
                    continue;
                }

                // Determine new size (how much we removed from backup)
                int newSize = (s32)ipb.length - ((s32)backupLocs[idx].endAddress - (s32)ipb.address);

                // Add split section after exclusion
                if (backupLocs[idx].endAddress > ipb.address + ipb.length)
                {
                    ssBackupLoc newLoc = {ipb.address + ipb.length, backupLocs[idx].endAddress, nullptr};
                    backupLocs.insert(backupLocs.begin() + idx + 1, newLoc);
                }

                // Modify section to end at the exclusion start
                backupLocs[idx].endAddress = ipb.address;
                if (backupLocs[idx].endAddress <= backupLocs[idx].startAddress)
                {
                    backupLocs.erase(backupLocs.begin() + idx);
                }

                // Set new size to see if there's still more to process
                newSize = newSize > 0 ? newSize : 0;
                ipb.address = ipb.address + (ipb.length - newSize);
                ipb.length = (u32)newSize;
            }
        }

        processedLocs.clear();
        processedLocs.insert(processedLocs.end(), backupLocs.begin(), backupLocs.end());
    }


    } // namespace Savestate





    namespace Mem
    {
        std::vector<u8> uint16ToVector(u16 num)
        {
            u8 byte0 = num >> 8;
            u8 byte1 = num & 0xFF;

            return std::vector<u8>({byte0, byte1});
        }

        std::vector<u8> uint32ToVector(u32 num)
        {
            u8 byte0 = num >> 24;
            u8 byte1 = (num & 0xFF0000) >> 16;
            u8 byte2 = (num & 0xFF00) >> 8;
            u8 byte3 = num & 0xFF;

            return std::vector<u8>({byte0, byte1, byte2, byte3});
        }

        std::vector<u8> int32ToVector(int32_t num)
        {
            u8 byte0 = num >> 24;
            u8 byte1 = (num & 0xFF0000) >> 16;
            u8 byte2 = (num & 0xFF00) >> 8;
            u8 byte3 = num & 0xFF;

            return std::vector<u8>({byte0, byte1, byte2, byte3});
        }

        void appendWordToBuffer(std::vector<u8>* buf, u32 word)
        {
            auto wordVector = uint32ToVector(word);
            buf->insert(buf->end(), wordVector.begin(), wordVector.end());
        }

        void appendHalfToBuffer(std::vector<u8>* buf, u16 word)
        {
            auto halfVector = uint16ToVector(word);
            buf->insert(buf->end(), halfVector.begin(), halfVector.end());
        }

        uint8_t readByte(uint8_t* a, int& idx, uint32_t maxSize, uint8_t defaultValue)
        {
            if (idx >= (int)maxSize)
            {
                idx += 1;
                return defaultValue;
            }

            return a[idx++];
        }

        uint16_t readHalf(uint8_t* a, int& idx, uint32_t maxSize, uint16_t defaultValue)
        {
            if (idx >= (int)maxSize)
            {
                idx += 2;
                return defaultValue;
            }

            uint16_t value = a[idx] << 8 | a[idx + 1];
            idx += 2;
            return value;
        }

        uint32_t readWord(uint8_t* a, int& idx, uint32_t maxSize, uint32_t defaultValue)
        {
            if (idx >= (int)maxSize)
            {
                idx += 4;
                return defaultValue;
            }

            uint32_t value = a[idx] << 24 | a[idx + 1] << 16 | a[idx + 2] << 8 | a[idx + 3];
            idx += 4;
            return value;
        }
        uint32_t readWord(uint8_t* a)
        {
            uint32_t value = a[0] << 24 | a[1] << 16 | a[2] << 8 | a[3];
            return value;
        }

        float readFloat(uint8_t* a, int& idx, uint32_t maxSize, float defaultValue)
        {
            uint32_t bytes = readWord(a, idx, maxSize, *(uint32_t*)(&defaultValue));
            return *(float*)(&bytes);
        }

    }  // namespace Mem


}
