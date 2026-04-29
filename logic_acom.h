#pragma once

void handleAcomUsb() {
  if (btnCancelPressed) {
    currentState = STATE_MAIN_MENU;
    lastActivityTime = millis();
    uiNeedsUpdate = true;
    return;
  }


  if (Serial.available() > 0) {
    String new_command = Serial.readStringUntil('\n');
    new_command.trim();

    if (new_command.length() > 0) {
      current_digirom = nullptr;
      // --- [NEW] DMComm Serial Printing Logic ---
      const char* digirom_str = new_command.c_str();
      DigiROMType rom_type = digiROMType(digirom_str);

      if (rom_type.signal_type != kSignalTypeInfo) {
        Serial.print(F("got "));
        Serial.print(new_command.length());
        Serial.print(F(" bytes: "));
        Serial.print(new_command);
        Serial.print(F(" -> "));
      }

      current_digirom = createDigiROM(digirom_str);

      // --- [NEW] DMComm Status Print ---
      if (rom_type.signal_type == kSignalTypeInfo) {
        // Serial.print(DMCOMM_BUILD_INFO); // We don't have this defined, skip for now
      } else if (current_digirom != nullptr) {
        Serial.print(F("(new DigiROM)"));
      } else {
        Serial.print(F("(paused)"));
      }
      Serial.println();
      // --- End Status Print ---
    }


    if (current_digirom != nullptr) {
      prevState = STATE_ACOM_USB;
      strncpy(selectedName, "ACOM USB Command", sizeof(selectedName) - 1);
      selectedName[sizeof(selectedName) - 1] = '\0';
      currentState = STATE_EXECUTING;
      executionStartTime = millis();
      lastExecutionTime = millis();
      strcpy(executionResult, "");
      lastExecutionResult[0] = '\0';
      textScrollOffset = 0;
      lastTextScrollTime = millis();
      uiNeedsUpdate = true;
    } else {
      // Serial.println("ACOM USB: Invalid ROM type received."); // Already printed "(paused)"
      strcpy(executionResult, "PAUSED");
      uiNeedsUpdate = true;
    }
  }
}