#pragma once

void populateDeviceMenu()
{
    deviceMenuCount = 0;
    int num_items = sizeof(digirom_data) / sizeof(digirom_data[0]);

    for (int i = 0; i < num_items && deviceMenuCount < MAX_DEVICES; i++)
    {
        bool found = false;
        if (digirom_data[i].device == nullptr) continue;

        for (int j = 0; j < deviceMenuCount; j++)
        {
            if (deviceMenu[j] != nullptr && strcmp(digirom_data[i].device, deviceMenu[j]) == 0)
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            deviceMenu[deviceMenuCount] = strdup(digirom_data[i].device);
            if (deviceMenu[deviceMenuCount] != nullptr) {
                deviceMenuCount++;
            } else {
                Serial.println("Error: Memory allocation failed for device name.");
            }
        }
    }
}


void populateNameMenu(const char *device)
{
    for (int i = 0; i < nameMenuCount; i++) {
         if (nameMenu[i] != nullptr) {
            free(nameMenu[i]);
            nameMenu[i] = nullptr; 
         }
    }

    nameMenuCount = 0;
    int num_items = sizeof(digirom_data) / sizeof(digirom_data[0]);

    for (int i = 0; i < num_items && nameMenuCount < MAX_NAMES_PER_DEVICE; i++)
    {
        if (digirom_data[i].device != nullptr && device != nullptr && strcmp(digirom_data[i].device, device) == 0)
        {
             if (digirom_data[i].name != nullptr) { 
                nameMenu[nameMenuCount] = strdup(digirom_data[i].name);
                if (nameMenu[nameMenuCount] != nullptr) { 
                    nameMenuCount++;
                } else {
                    Serial.println("Error: Failed to allocate memory for ROM name.");
                    break; 
                }
            }
        }
    }
}


const char *findDigiROMCode(const char *device, const char *name)
{
    int num_items = sizeof(digirom_data) / sizeof(digirom_data[0]);
    if (device == nullptr || name == nullptr) return nullptr;

    for (int i = 0; i < num_items; i++)
    {
        if (digirom_data[i].device != nullptr && digirom_data[i].name != nullptr &&
            strcmp(digirom_data[i].device, device) == 0 &&
            strcmp(digirom_data[i].name, name) == 0)
        {
            return digirom_data[i].code;
        }
    }
    return nullptr;
}


void handleDeviceMenu()
{
    const int maxItemsOnScreen = 3;
    if (btnUpPressed)
    {
        deviceMenuCursor--;
        if (deviceMenuCursor < 0) deviceMenuCursor = deviceMenuCount -1; 
        if (deviceMenuCursor < deviceMenuWindowStart) {
            deviceMenuWindowStart = deviceMenuCursor;
        }
        else if (deviceMenuCursor == deviceMenuCount - 1 && deviceMenuCount > maxItemsOnScreen){
             deviceMenuWindowStart = deviceMenuCount - maxItemsOnScreen;
        }
        textScrollOffset = 0;
        lastTextScrollTime = millis();
        uiNeedsUpdate = true;
    }
    if (btnDownPressed)
    {
        deviceMenuCursor++;
        if (deviceMenuCursor >= deviceMenuCount) deviceMenuCursor = 0; 
        if (deviceMenuCursor >= deviceMenuWindowStart + maxItemsOnScreen) {
            deviceMenuWindowStart = deviceMenuCursor - (maxItemsOnScreen - 1);
        }
        else if (deviceMenuCursor == 0 && deviceMenuCount > maxItemsOnScreen){
            deviceMenuWindowStart = 0;
        }
        textScrollOffset = 0;
        lastTextScrollTime = millis();
        uiNeedsUpdate = true;
    }
    if (btnOkPressed)
    {
        if (deviceMenuCount > 0 && deviceMenu[deviceMenuCursor] != nullptr) { 
            strcpy(selectedDevice, deviceMenu[deviceMenuCursor]);
            populateNameMenu(selectedDevice); 
            nameMenuCursor = 0;
            nameMenuWindowStart = 0;
            currentState = STATE_DIGIROM_NAME;
            textScrollOffset = 0;
            lastTextScrollTime = millis();
            uiNeedsUpdate = true;
        }
    }
    if (btnCancelPressed)
    {
        currentState = STATE_MAIN_MENU;
        lastActivityTime = millis(); 
        textScrollOffset = 0;
        lastTextScrollTime = millis();
        uiNeedsUpdate = true;
    }
}

void handleNameMenu()
{
    const int maxItemsOnScreen = 3;
    if (btnUpPressed)
    {
        nameMenuCursor--;
        if (nameMenuCursor < 0) nameMenuCursor = nameMenuCount - 1; 
        if (nameMenuCursor < nameMenuWindowStart) {
            nameMenuWindowStart = nameMenuCursor;
        } else if (nameMenuCursor == nameMenuCount - 1 && nameMenuCount > maxItemsOnScreen) {
            nameMenuWindowStart = nameMenuCount - maxItemsOnScreen;
        }
        textScrollOffset = 0;
        lastTextScrollTime = millis();
        uiNeedsUpdate = true;
    }
    if (btnDownPressed)
    {
        nameMenuCursor++;
        if (nameMenuCursor >= nameMenuCount) nameMenuCursor = 0; 
        if (nameMenuCursor >= nameMenuWindowStart + maxItemsOnScreen) {
            nameMenuWindowStart = nameMenuCursor - (maxItemsOnScreen - 1);
        } else if (nameMenuCursor == 0 && nameMenuCount > maxItemsOnScreen) {
            nameMenuWindowStart = 0;
        }
        textScrollOffset = 0;
        lastTextScrollTime = millis();
        uiNeedsUpdate = true;
    }
    if (btnOkPressed)
    {
         if (nameMenuCount > 0 && nameMenu[nameMenuCursor] != nullptr) { 
            strcpy(selectedName, nameMenu[nameMenuCursor]);
            const char *code = findDigiROMCode(selectedDevice, selectedName);
            if (code != nullptr)
            {
                if (current_digirom != nullptr) delete current_digirom;
                current_digirom = createDigiROM(code);

                if (current_digirom != nullptr) {
                    prevState = STATE_DIGIROM_NAME; 
                    currentState = STATE_EXECUTING;
                    executionStartTime = millis();
                    lastExecutionTime = millis(); 
                    strcpy(executionResult, ""); 
                    lastExecutionResult[0] = '\0'; 
                    textScrollOffset = 0;
                    lastTextScrollTime = millis(); 
                    uiNeedsUpdate = true;
                } else {
                    Serial.println("Error creating DigiROM");
                }
            } else {
                Serial.println("Error finding DigiROM code");
            }
        }
    }
    if (btnCancelPressed)
    {
        currentState = STATE_DIGIROM_DEVICE; 
        textScrollOffset = 0;
        lastTextScrollTime = millis();
        uiNeedsUpdate = true;
    }
}