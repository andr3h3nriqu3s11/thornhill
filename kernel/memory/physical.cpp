#include <algorithm>
#include <math>
#include <thornhill>

#include "physical.hpp"
#include "kernel/arch/x86_64/include.hpp"

using namespace std;
using namespace Thornhill;

namespace ThornhillMemory {

    /**
     * @brief Returns the first available page with a base address no lower
     * than the specified minimum base address.
     *
     * Returns null (0) if the specified page couldn't be found.
     *
     * @param bootMap The boot-time memory map that is passed into the PMM
     * initialization method.
     * @param minBaseAddress The lowest base address of a page that should
     * be returned.
     * @return The base address of the page with the lowest memory address
     * still meeting the specified criteria.
     */
    static uint64_t _firstAvailablePage(HandoffMemoryMap& bootMap, uint64_t minBaseAddress = 1) {
        assert(minBaseAddress > 0);
        uint64_t baseAddress = null;

        for (uintptr_t segmentIndex = 0; segmentIndex < bootMap.mapSize; segmentIndex++) {
            if (bootMap.segments[segmentIndex].memoryType == THAvailableMemory &&
                bootMap.segments[segmentIndex].pageCount >= 1 &&
                bootMap.segments[segmentIndex].physicalBaseAddress >= minBaseAddress) {

                // Grab the smallest non-zero base address.
                if (baseAddress == null || baseAddress > bootMap.segments[segmentIndex].physicalBaseAddress) {
                    baseAddress = bootMap.segments[segmentIndex].physicalBaseAddress;
                }

            }
        }

        return baseAddress;
    }

    static uint64_t _lastAvailablePage(HandoffMemoryMap& bootMap) {
        uint64_t baseAddress = null;

        for (uintptr_t segmentIndex = 0; segmentIndex < bootMap.mapSize; segmentIndex++) {
            if (bootMap.segments[segmentIndex].memoryType == THAvailableMemory &&
                bootMap.segments[segmentIndex].pageCount >= 1 &&
                bootMap.segments[segmentIndex].physicalBaseAddress >= 1) {

                // Grab the largest base address of that segment's pages.
                uint64_t largestBaseAddress = bootMap.segments[segmentIndex].physicalBaseAddress;

                if (largestBaseAddress > baseAddress) {
                    baseAddress = largestBaseAddress;
                }

            }
        }

        return baseAddress;
    }

    /**
     * Returns the boot map's segment index of the segment of the page that the specified address
     * belongs to.
     *
     * Panics if the segment index couldn't be found.
     *
     * @param bootMap The boot-time memory map that is passed into the PMM
     * initialization method.
     * @param address The address that the segment should contain.
     * @return The integer memory segment ID in bootMap that the address belongs to.
     */
    static uint64_t _bootMapSegmentForPageAddress(HandoffMemoryMap& bootMap, uint64_t address) {
        for (uintptr_t segmentIndex = 0; segmentIndex < bootMap.mapSize; segmentIndex++) {
            if (
                bootMap.segments[segmentIndex].physicalBaseAddress >= address &&
                address < (bootMap.segments[segmentIndex].physicalBaseAddress + PAGES(bootMap.segments[segmentIndex].pageCount))
            ) {
                return segmentIndex;
            }
        }

        Kernel::panic("There was a problem interpreting data from the firmware.");
    }

    bool Physical::initialized = false;
    size_t Physical::usedMemory = 0;
    size_t Physical::totalMemory = 0;
    size_t Physical::inventoryBase = 0;

    void Physical::reset() {
        Physical::initialized = false;
        Physical::usedMemory = 0;
        Physical::totalMemory = 0;
        Physical::inventoryBase = 0;
    }

    void Physical::initialize(HandoffMemoryMap bootMap) {
        Kernel::debug("PMM", "Initializing physical memory manager...");

        if (Physical::initialized) {
            Kernel::panic("Attempted to re-initialize PMM.");
        }

        // Take inventory.
        {
            Kernel::debug("PMM", "Taking inventory of memory structures...");

            // Locate the first available page.
            uint64_t firstPage = _firstAvailablePage(bootMap);
            // ...and locate the last available page.
            uint64_t lastPage = _lastAvailablePage(bootMap);

            if (firstPage == null) {
                // If we failed to find a page (indicated by firstPage being null), we should panic
                // because this means there's absolutely no pages available, meaning there's no
                // available system memory at all.
                Kernel::panic("No available memory.");
            }

            // Take the first page for PMM inventory usage and set inventoryBase to its
            // base address.
            Kernel::debugf("PMM", "Taking page (base = %x) for PMM usage.", firstPage);
            Physical::inventoryBase = firstPage;
            assert(Physical::inventoryBase != null);


            ThornhillPhysicalFrame* previousFrame = nullptr;
            // Take the first page as the inventory page.
            uint64_t inventoryPage = firstPage;
            // ...and initialize the current frame to be the first possible frame in the inventory
            // page.
            auto* currentFrame = (ThornhillPhysicalFrame*)inventoryPage;
            // Now, initialize the current page as the next available page after the inventory page.
            uint64_t currentPage = _firstAvailablePage(bootMap, inventoryPage + PAGES(1));

            while (currentPage <= lastPage) {
                if (currentPage == null) {
                    Kernel::debug("PMM", "    No more usable pages.");
                    break;
                }

                // Load the firmware information about the current page.
                uint64_t currentSegmentIndex = _bootMapSegmentForPageAddress(bootMap, currentPage);
                HandoffMemorySegment segment = bootMap.segments[currentSegmentIndex];
                // Handle updating the physical base address for the current page.
                if (currentPage == Physical::inventoryBase + PAGES(1)) {
                    segment.physicalBaseAddress = currentPage;
                }

                // Initialize the frame.
                currentFrame->base = segment.physicalBaseAddress;
                currentFrame->metadata = 0;

                // Calculate the size of the bitmap required to hold this contiguous section of
                // pages.
                uint16_t bitmapSize = max(
                    (uint64_t) min(
                        (uint64_t) ceilToN((long double) segment.pageCount / 8, 8),
                        (inventoryPage + PAGES(1)) - ((uint64_t)(currentFrame) + PHYSICAL_FRAME_HEADER_SIZE)
                    ),
                    0UL
                );

                // Subtract the page count.
                segment.pageCount -= bitmapSize * 8;
                segment.physicalBaseAddress += PAGES(bitmapSize * 8);

                // Load the bitmap base address (which is the current frame offset by the page
                // size.)
                auto* bitmap = (uint8_t*)(currentFrame + PHYSICAL_FRAME_HEADER_SIZE);
                // Zero the bitmap.
                memzero(bitmap, bitmapSize);

                // Determine whether the current bitmap will cause the inventory page to be filled.
                bool didFillInventoryPage = (uint64_t) bitmap + bitmapSize == (uint64_t) inventoryPage + PAGES(1) - 1;

                // Update the current frame to reflect the newly defined bitmap.
                currentFrame->count = bitmapSize * 8;

                Kernel::debugf("PMM", "    Adding %d page(s) (base = %x).",
                               currentFrame->count,
                               currentFrame->base);
                currentPage += PAGES(currentFrame->count);

                // Reset the current frame, update the previous frame if it exists and then replace
                // the 'previousFrame' reference with the current frame.
                if (previousFrame != nullptr)
                    previousFrame->next = reinterpret_cast<uint64_t>(currentFrame);
                previousFrame = currentFrame;

                // If the inventory page was filled, we need to select a new inventory page to
                // continue taking inventory.
                if (didFillInventoryPage) {
                    // Take the next available page as an inventory page.
                    inventoryPage = _firstAvailablePage(bootMap, currentPage + PAGES(1));

                    // If an inventory page couldn't be found, break early as we've exhausted available memory.
                    if (inventoryPage == null)
                        break;

                    segment.physicalBaseAddress += PAGES(2);
                    segment.pageCount--;
                }
                // Otherwise, offset the inventory page.
                else {
                    currentFrame += PHYSICAL_FRAME_HEADER_SIZE + bitmapSize;
                    currentPage = _firstAvailablePage(bootMap, currentPage + PAGES(1));
                }
            }
        }

        // Check inventory.
        {
            Kernel::debug("PMM", "Checking inventory...");
            auto* currentFrame = reinterpret_cast<ThornhillPhysicalFrame*>(Physical::inventoryBase);

            while (currentFrame != null) {
                Physical::totalMemory += PAGES(currentFrame->count);
                currentFrame = reinterpret_cast<ThornhillPhysicalFrame*>(currentFrame->next);
            }
        }

        if (Physical::totalMemory < 1) {
            Kernel::panic(
                "Out of Memory",
                SYSTEM_SELF_PANIC_INTERRUPT_NUMBER,
                "Physical memory manager was unable to find any available physical memory pages."
            );
        }

        Physical::initialized = true;
        Kernel::debug("PMM", "PMM is ready.");
        Kernel::debugf("PMM", "    Physical memory: %u MiB (%u KiB, %u bytes)", Physical::totalMemory / 1024 / 1024, Physical::totalMemory / 1024, Physical::totalMemory);
    }

    void* Physical::allocate(size_t pageCount) {
        Kernel::debugf("PMM", "Allocating %u pages...", pageCount);
        auto* currentFrame = reinterpret_cast<ThornhillPhysicalFrame*>(Physical::inventoryBase);

        while (currentFrame != null) {
            // The base of the current frame.
            uint64_t base = currentFrame->base;
            // The offset from [base] that the contiguous chunk of memory starts.
            uint64_t baseOffset = 0;
            // The number of presently found contiguous pages after baseOffset.
            uint64_t contiguous = 0;

            for (uint64_t pageGroup = 0; pageGroup < currentFrame->count; pageGroup++) {
                if (contiguous >= pageCount) {
                    // TODO: mark allocated.
                    for (uint64_t i = 0; i < pageCount; i++) {
                        // Get the bitmap entry based on the floored-by-8 page index.
                        uint64_t bitmapGroupBase = ((baseOffset / TH_ARCH_PAGE_SIZE) + i) / 8;
                        uint64_t bitmapGroupOffset = ((baseOffset / TH_ARCH_PAGE_SIZE) + i) % 8;

                        currentFrame->bitmap[bitmapGroupBase] |= 1 << (7 - bitmapGroupOffset);
                        usedMemory += TH_ARCH_PAGE_SIZE;
                    }

                    Kernel::debugf("PMM", "Allocated %u pages. (Total: %u, Used Mem: %u)", pageCount, usedMemory / TH_ARCH_PAGE_SIZE, usedMemory);
                    return (void*) (base + baseOffset);
                }

                // If the entire group in the bitmap is free, then mark the entire group as
                // free contiguous pages.
                if (currentFrame->bitmap[pageGroup] == 0) contiguous += 8;
                // Otherwise, there is an entry in the bitmap that is not free, so we need to
                // start counting from the first free page.
                else {
                    // Figure out the 0-indexed location of the first free page in this group.
                    uint8_t location = 0;

                    for (uint8_t check = 8; check <= 8; check--) {
                        if ((currentFrame->bitmap[pageGroup] & (0b1 << check)) > 0) {
                            location = 8 - check;
                        }
                    }

                    // If there is no free page in this bitmap group, skip it and continue to
                    // the next one.
                    assert(location != 0);
                    if (location >= 8) {
                        contiguous = 0;
                        baseOffset = PAGES((pageGroup + 1) * 8);
                        continue;
                    }

                    // Now add contiguous pages and set baseOffset.
                    baseOffset = PAGES((pageGroup * 8) + location);
                    contiguous = 8 - location;
                }
            }

            currentFrame = reinterpret_cast<ThornhillPhysicalFrame*>(currentFrame->next);
        }

        Kernel::debugf("PMM", "Failed to allocate %u pages.", pageCount);
        return nullptr;
    }

    [[maybe_unused]] void Physical::deallocate([[maybe_unused]] void* base, [[maybe_unused]] size_t pageCount) {
        Kernel::panic("Tried to deallocate, but wasn't yet implemented.");
    }

} // namespace ThornhillMemory
