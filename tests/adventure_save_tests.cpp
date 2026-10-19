#include "nuby/adventure_save.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

nuby::AdventureSaveData validSave() {
    nuby::AdventureSaveData save;
    save.playerX = -4.25f;
    save.playerZ = -7.5f;
    save.playerYaw = 0.65f;
    save.cameraYaw = -1.2f;
    save.cameraPitch = 0.42f;
    save.collectedRelicMask = 0x05U;
    return save;
}

void testRoundTripAndStableWorldIdentity() {
    const nuby::AdventureSaveData input = validSave();
    std::string serialized;
    std::string error;
    expect(nuby::serializeAdventureSave(input, serialized, error),
           "valid adventure progress serializes");
    expect(serialized.find("WORLD sun_ruins\n") != std::string::npos,
           "save is tied to the authored world identity");
    nuby::AdventureSaveData output;
    expect(nuby::parseAdventureSave(serialized, output, error),
           "serialized progress parses");
    expect(output.playerX == input.playerX && output.playerZ == input.playerZ
                   && output.playerYaw == input.playerYaw
                   && output.cameraYaw == input.cameraYaw
                   && output.cameraPitch == input.cameraPitch
                   && output.collectedRelicMask == input.collectedRelicMask
                   && !output.completed,
           "round trip preserves player, camera, relics, and completion state");
}

void testCompletedSaveRequiresRelicsAndGatePosition() {
    nuby::AdventureSaveData save = validSave();
    save.playerX = 0.0f;
    save.playerZ = -11.5f;
    save.collectedRelicMask = 0x07U;
    save.completed = true;
    std::string serialized;
    std::string error;
    expect(nuby::serializeAdventureSave(save, serialized, error),
           "completed save at the gate with all relics is valid");

    save.collectedRelicMask = 0x03U;
    expect(!nuby::serializeAdventureSave(save, serialized, error),
           "completion cannot be saved before all relics are collected");
    save.collectedRelicMask = 0x07U;
    save.playerX = 8.0f;
    expect(!nuby::serializeAdventureSave(save, serialized, error),
           "completion cannot be saved away from the gate");
}

void testMalformedOrTamperedSavesAreRejectedTransactionally() {
    std::string serialized;
    std::string error;
    expect(nuby::serializeAdventureSave(validSave(), serialized, error),
           "fixture serializes before tamper checks");
    nuby::AdventureSaveData output;
    output.playerX = 12.0f;
    output.collectedRelicMask = 0x02U;
    const nuby::AdventureSaveData original = output;
    std::string tampered = serialized;
    const std::size_t playerValue = tampered.find("PLAYER ");
    expect(playerValue != std::string::npos, "fixture has a player record");
    if (playerValue != std::string::npos) {
        const std::size_t digit = tampered.find("4.250000", playerValue);
        if (digit != std::string::npos) tampered[digit] = '3';
    }
    expect(!nuby::parseAdventureSave(tampered, output, error),
           "edited save content fails checksum validation");
    expect(output.playerX == original.playerX
                   && output.collectedRelicMask == original.collectedRelicMask,
           "failed load leaves caller state unchanged");
    expect(!nuby::parseAdventureSave("NUBY_ADVENTURE_SAVE 999\n", output, error),
           "unsupported or truncated save versions are rejected");
    expect(!nuby::parseAdventureSave(std::string(4097U, 'x'), output, error),
           "oversized saves are rejected before parsing");
}

void testInvalidTransformsAndRelicMasksAreRejected() {
    nuby::AdventureSaveData save = validSave();
    std::string serialized;
    std::string error;
    save.playerX = std::numeric_limits<float>::infinity();
    expect(!nuby::serializeAdventureSave(save, serialized, error),
           "non-finite player positions are rejected");
    save = validSave();
    save.cameraPitch = 3.0f;
    expect(!nuby::serializeAdventureSave(save, serialized, error),
           "camera pitch outside controller bounds is rejected");
    save = validSave();
    save.collectedRelicMask = 0x08U;
    expect(!nuby::serializeAdventureSave(save, serialized, error),
           "unknown relic bits are rejected");
}

}  // namespace

int main() {
    testRoundTripAndStableWorldIdentity();
    testCompletedSaveRequiresRelicsAndGatePosition();
    testMalformedOrTamperedSavesAreRejectedTransactionally();
    testInvalidTransformsAndRelicMasksAreRejected();
    if (failures != 0) {
        std::cerr << failures << " adventure save test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All adventure save tests passed\n";
    return EXIT_SUCCESS;
}
