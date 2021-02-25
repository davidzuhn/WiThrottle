/* -*- c++ -*-
 *
 * WiThrottleProtocolProtocol
 *
 * This package implements a WiThrottleProtocol protocol connection,
 * allow a device to communicate with a JMRI server or other
 * WiThrottleProtocol device (like the Digitrax LNWI).
 *
 * Copyright © 2018-2019, 2021 Blue Knobby Systems Inc.
 *
 * This work is licensed under the Creative Commons Attribution-ShareAlike
 * 4.0 International License. To view a copy of this license, visit
 * http://creativecommons.org/licenses/by-sa/4.0/ or send a letter to
 * Creative Commons, PO Box 1866, Mountain View, CA 94042, USA.
 *
 * Attribution — You must give appropriate credit, provide a link to the
 * license, and indicate if changes were made. You may do so in any
 * reasonable manner, but not in any way that suggests the licensor
 * endorses you or your use.
 *
 * ShareAlike — If you remix, transform, or build upon the material, you
 * must distribute your contributions under the same license as the
 * original.
 *
 * All other rights reserved.
 *
 */

#include <ArduinoTime.h>
#include <TimeLib.h>

#include "WiThrottleProtocol.h"


#define NEWLINE '\n'
#define CR '\r'
#define PROPERTY_SEPARATOR "<;>"

static const int MIN_SPEED = 0;
static const int MAX_SPEED = 126;


WiThrottleProtocol::WiThrottleProtocol(bool server):
    server(server),
    heartbeatTimer(Chrono::SECONDS),
    fastTimeTimer(Chrono::SECONDS),
    currentSpeed(0),
    speedSteps(0),
    currentDirection(Forward)
{
    init();
}

void
WiThrottleProtocol::init()
{
    stream = NULL;
    memset(inputbuffer, 0, sizeof(inputbuffer));
    nextChar = 0;
    heartbeatPeriod = 0;
    currentFastTime = 0.0;
    currentFastTimeRate = 0.0;
    locomotiveSelected = false;
    resetChangeFlags();
}

void
WiThrottleProtocol::begin(Stream *console)
{
    this->console = console;
}


void
WiThrottleProtocol::resetChangeFlags()
{
    clockChanged = false;
    heartbeatChanged = false;
}

void
WiThrottleProtocol::connect(Stream *stream)
{
    init();
    this->stream = stream;
}

void
WiThrottleProtocol::disconnect()
{
    this->stream = NULL;
}

void
WiThrottleProtocol::setDeviceName(String deviceName)
{
    String command = "N" + deviceName;
    sendCommand(command);
}

void
WiThrottleProtocol::setDeviceID(String deviceId)
{
    String command = "H" + deviceId;
    sendCommand(command);
}


bool
WiThrottleProtocol::check()
{
    bool changed = false;
    resetChangeFlags();

    if (stream) {
        // update the fast clock first
        changed |= checkFastTime();
        changed |= checkHeartbeat();

        while(stream->available()) {
            char b = stream->read();
            if (b == NEWLINE | b==CR) {
                // server sends TWO newlines after each command, we trigger on the
                // first, and this skips the second one
                if (nextChar != 0) {
                    inputbuffer[nextChar] = 0;
                    changed |= processCommand(inputbuffer, nextChar);
                }
                nextChar = 0;
            }
            else {
                inputbuffer[nextChar] = b;
                nextChar += 1;
                if (nextChar == 1023) {
                    inputbuffer[1023] = 0;
                    console->print("ERROR LINE TOO LONG: ");
                    console->println(inputbuffer);
                    nextChar = 0;
                }
            }
        }

        return changed;

    }
    else {
        return false;
    }
}

void
WiThrottleProtocol::sendCommand(String cmd)
{
    if (stream) {
        // TODO: what happens when the write fails?
        stream->println(cmd);
        if (server) {
            stream->println("");
        }
        console->print("==> "); console->println(cmd);
    }
}


bool
WiThrottleProtocol::checkFastTime()
{
    bool changed = true;
    if (fastTimeTimer.hasPassed(1)) { // one real second
        fastTimeTimer.restart();
        if (currentFastTimeRate == 0.0) {
            clockChanged = false;
        }
        else {
            currentFastTime += currentFastTimeRate;
            clockChanged = true;
        }
    }

    return changed;
}


int
WiThrottleProtocol::fastTimeHours()
{
    time_t now = (time_t) currentFastTime;
    return hour(now);
}


int
WiThrottleProtocol::fastTimeMinutes()
{
    time_t now = (time_t) currentFastTime;
    return minute(now);
}


float
WiThrottleProtocol::fastTimeRate()
{
    return currentFastTimeRate;
}


bool
WiThrottleProtocol::processLocomotiveAction(char *c, int len)
{
    String remainder(c);  // the leading "MTA" was not passed to this method

    //console->printf("processLocomotiveAction: remainder at first is %s\n", remainder.c_str());

    if (currentAddress.equals("")) {
        //console->printf("  skipping due to no selected address\n");
        return true;
    }
    else {
        //console->printf("  currentAddress is '%s'\n", currentAddress.c_str());
    }

    String addrCheck = currentAddress + PROPERTY_SEPARATOR;
    String allCheck = "*";
    allCheck.concat(PROPERTY_SEPARATOR);
    if (remainder.startsWith(addrCheck)) {
        remainder.remove(0, addrCheck.length());
    }
    else if (remainder.startsWith(allCheck)) {
        remainder.remove(0, allCheck.length());
    }

    //console->printf("processLocomotiveAction: after separator is %s\n", remainder.c_str());

    if (remainder.length() > 0) {
        char action = remainder[0];

        switch (action) {
            case 'F':
                //console->printf("processing function state\n");
                processFunctionState(remainder);
                break;
            case 'V':
                processSpeed(remainder);
                break;
            case 's':
                processSpeedSteps(remainder);
                break;
            case 'R':
                processDirection(remainder);
                break;
            default:
                console->printf("unrecognized action '%c'\n", action);
                // no processing on unrecognized actions
                break;
        }
        return true;
    }
    else {
        console->printf("insufficient action to process\n");
        return false;
    }
}



bool
WiThrottleProtocol::processCommand(char *c, int len)
{
    bool changed = false;

    console->print("<== ");
    console->println(c);

    // we regularly get this string as part of the data sent
    // by a Digitrax LnWi.  Remove it, and try again.
    const char *ignoreThisGarbage = "AT+CIPSENDBUF=";
    while (strncmp(c, ignoreThisGarbage, strlen(ignoreThisGarbage)) == 0) {
        console->printf("removed one instance of %s\n", ignoreThisGarbage);
        c += strlen(ignoreThisGarbage);
        changed = true;
    }

    if (changed) {
        console->printf("input string is now: '%s'\n", c);
    }

    if (len > 3 && c[0]=='P' && c[1]=='F' && c[2]=='T') {
        return processFastTime(c+3, len-3);
    }
    else if (len > 3 && c[0]=='P' && c[1]=='P' && c[2]=='A') {
        processTrackPower(c+3, len-3);
        return true;
    }
    else if (len > 1 && c[0]=='*') {
        return processHeartbeat(c+1, len-1);
    }
    else if (len > 2 && c[0]=='V' && c[1]=='N') {
        processProtocolVersion(c+2, len-2);
        return true;
    }
    else if (len > 2 && c[0]=='P' && c[1]=='W') {
        processWebPort(c+2, len-2);
        return true;
    }
    else if (len > 6 && c[0]=='M' && c[1]=='T' && c[2]=='S') {
        processStealNeeded(c+3, len-3);
        return true;
    }
    else if (len > 6 && c[0]=='M' && c[1]=='T' && (c[2]=='+' || c[2]=='-')) {
        // we want to make sure the + or - is passed in as part of the string to process
        processAddRemove(c+2, len-2);
        return true;
    }
    else if (len > 8 && c[0]=='M' && c[1]=='T' && c[2]=='A') {
        return processLocomotiveAction(c+3, len-3);
    }
    else if (len > 3 && c[0]=='A' && c[1]=='T' && c[2]=='+') {
        // this is an AT+.... command that the LnWi sometimes emits and we
        // ignore these commands altogether
    }
    else {
        console->printf("unknown command '%s'\n", c);
        // all other commands are explicitly ignored
    }
}



void
WiThrottleProtocol::setCurrentFastTime(const String& s)
{
    int t = s.toInt();
    if (currentFastTime == 0.0) {
        console->print("set fast time to "); console->println(t);
    }
    else {
        console->print("updating fast time (should be "); console->print(t);
        console->print(" is "); console->print(currentFastTime);  console->println(")");
        console->printf("currentTime is %d\n", millis());
    }
    currentFastTime = t;
}


bool
WiThrottleProtocol::processFastTime(char *c, int len)
{
    // keep this style -- I don't validate the settings and syntax
    // as well as I could, so someday we might return false

    bool changed = false;

    String s(c);

    int p = s.indexOf(PROPERTY_SEPARATOR);
    if (p > 0) {
        String timeval = s.substring(0, p);
        String rate    = s.substring(p+3);

        setCurrentFastTime(timeval);
        currentFastTimeRate = rate.toFloat();
        console->print("set clock rate to "); console->println(currentFastTimeRate);
        changed = true;
        clockChanged = true;
    }
    else {
        setCurrentFastTime(s);
        changed = true;
    }

    return changed;
}


bool
WiThrottleProtocol::processHeartbeat(char *c, int len)
{
    bool changed = false;
    String s(c);

    heartbeatPeriod = s.toInt();
    if (heartbeatPeriod > 0) {
        heartbeatChanged = true;
        changed = true;
        if (delegate) {
            delegate->heartbeatConfig(heartbeatPeriod);
        }
    }
    return changed;
}


void
WiThrottleProtocol::processProtocolVersion(char *c, int len)
{
    if (delegate && len > 0) {
        String protocolVersion = String(c);
        delegate->receivedVersion(protocolVersion);
    }
}

void
WiThrottleProtocol::processWebPort(char *c, int len)
{
    if (delegate && len > 0) {
        String port_string = String(c);
        int port = port_string.toInt();

        delegate->receivedWebPort(port);
    }
}



// the string passed in will look 'F03' (meaning turn off Function 3) or
// 'F112' (turn on function 12)
void
WiThrottleProtocol::processFunctionState(const String& functionData)
{
    // F[0|1]nn - where nn is 0-28
    if (delegate && functionData.length() >= 3) {
        bool state = functionData[1]=='1' ? true : false;

        String funcNumStr = functionData.substring(2);
        uint8_t funcNum = funcNumStr.toInt();

        if (funcNum == 0 && funcNumStr != "0") {
            // error in parsing
        }
        else {
            delegate->receivedFunctionState(funcNum, state);
        }
    }
}


void
WiThrottleProtocol::processSpeed(const String& speedData)
{
    if (delegate && speedData.length() >= 2) {
        String speedStr = speedData.substring(1);
        int speed = speedStr.toInt();

        if ((speed < MIN_SPEED) || (speed > MAX_SPEED)) {
            speed = 0;
        }

        delegate->receivedSpeed(speed);
    }
}


void
WiThrottleProtocol::processSpeedSteps(const String& speedStepData)
{
    if (delegate && speedStepData.length() >= 2) {
        String speedStepStr = speedStepData.substring(1);
        int steps = speedStepStr.toInt();

        if (steps != 1 && steps != 2 && steps != 4 && steps != 8 && steps !=16) {
            // error, not one of the known values
        }
        else {
            delegate->receivedSpeedSteps(steps);
        }
    }
}


void
WiThrottleProtocol::processDirection(const String& directionStr)
{
    console->print("DIRECTION STRING: ");
    console->println(directionStr);
    console->print("LENGTH: ");
    console->println(directionStr.length());


    // R[0|1]
    if (delegate && directionStr.length() == 2) {
        if (directionStr.charAt(1) == '0') {
            currentDirection = Reverse;
        }
        else {
            currentDirection = Forward;
        }

        delegate->receivedDirection(currentDirection);
    }
}



void
WiThrottleProtocol::processTrackPower(char *c, int len)
{
    if (delegate) {
        if (len > 0) {
            TrackPower state = PowerUnknown;
            if (c[0]=='0') {
                state = PowerOff;
            }
            else if (c[0]=='1') {
                state = PowerOn;
            }

            delegate->receivedTrackPower(state);
        }
    }
}


void
WiThrottleProtocol::processAddRemove(char *c, int len)
{
    if (!delegate) {
        // If no one is listening, don't do the work to parse the string
        return;
    }

    //console->printf("processing add/remove command %s\n", c);

    String s(c);

    bool add = (c[0] == '+');
    bool remove = (c[0] == '-');

    int p = s.indexOf(PROPERTY_SEPARATOR);
    if (p > 0) {
        String address = s.substring(1, p);
        String entry   = s.substring(p+3);

        address.trim();
        entry.trim();

        if (add) {
            delegate->addressAdded(address, entry);
        }
        if (remove) {
            if (entry.equals("d\n") || entry.equals("r\n")) {
                delegate->addressRemoved(address, entry);
            }
            else {
                console->printf("malformed address removal: command is %s\n", entry.c_str());
                console->printf("entry length is %d\n", entry.length());
                for (int i = 0; i < entry.length(); i++) {
                    console->printf("  char at %d is %d\n", i, entry.charAt(i));
                }
            }
        }
    }


}


void
WiThrottleProtocol::processStealNeeded(char *c, int len)
{
    if (!delegate) {
        // If no one is listening, don't do the work to parse the string
        return;
    }

    console->printf("processing steal needed command %s\n", c);

    String s(c);

    int p = s.indexOf(PROPERTY_SEPARATOR);
    if (p > 0) {
        String address = s.substring(0, p);
        String entry   = s.substring(p+3);

        delegate->addressStealNeeded(address, entry);
    }
}




bool
WiThrottleProtocol::checkHeartbeat()
{
    if (heartbeatPeriod > 0 && heartbeatTimer.hasPassed(0.5 * heartbeatPeriod)) {
        heartbeatTimer.restart();

        sendCommand("*");
        return true;
    }
    else {
        return false;
    }
}


void
WiThrottleProtocol::requireHeartbeat(bool needed)
{
    if (needed) {
        sendCommand("*+");
    }
    else {
        sendCommand("*-");
    }
}

bool
WiThrottleProtocol::addLocomotive(String address)
{
    bool ok = false;

    if (address[0] == 'S' || address[0] == 'L') {
        String rosterName = address;  // for now -- could look this up...
        String cmd = "MT+" + address + PROPERTY_SEPARATOR + rosterName;
        sendCommand(cmd);

        currentAddress = address;
        ok = true;

        locomotiveSelected = true;
    }

    return ok;
}


bool
WiThrottleProtocol::stealLocomotive(String address)
{
    bool ok = false;

    if (releaseLocomotive(address)) {
        ok = addLocomotive(address);
    }

    return ok;
}


bool
WiThrottleProtocol::releaseLocomotive(String address)
{
    // MT-*<;>r
    String cmd = "MT-";
    cmd.concat(address);
    cmd.concat(PROPERTY_SEPARATOR);
    cmd.concat("r");
    sendCommand(cmd);

    locomotiveSelected = false;

    return true;
}


bool
WiThrottleProtocol::setSpeed(int speed)
{
    if (speed < 0 || speed > 126) {
        return false;
    }
    if (!locomotiveSelected) {
        return false;
    }

    if (speed != currentSpeed) {
        String cmd = "MTA*";
        cmd.concat(PROPERTY_SEPARATOR);
        cmd.concat("V");
        cmd.concat(String(speed));
        sendCommand(cmd);
        currentSpeed = speed;
    }
    return true;
}


int
WiThrottleProtocol::getSpeed()
{
    return currentSpeed;
}


bool
WiThrottleProtocol::setDirection(Direction direction)
{
    if (!locomotiveSelected) {
        return false;
    }


    String cmd = "MTA*";
    cmd.concat(PROPERTY_SEPARATOR);
    cmd.concat("R");
    if (direction == Reverse) {
        cmd += "0";
    }
    else {
        cmd += "1";
    }
    sendCommand(cmd);

    currentDirection = direction;
    return true;
}


Direction
WiThrottleProtocol::getDirection()
{
    return currentDirection;
}


void
WiThrottleProtocol::emergencyStop()
{
    String cmd = "MTA*";
    cmd.concat(PROPERTY_SEPARATOR);
    cmd.concat("X");

    sendCommand(cmd);
}


void
WiThrottleProtocol::setFunction(int funcNum, bool pressed)
{
    if (!locomotiveSelected) {
        return;
    }

    if (funcNum < 0 || funcNum > 28) {
        return;
    }

    String cmd = "MTA";
    cmd.concat(currentAddress);
    cmd.concat(PROPERTY_SEPARATOR);
    cmd.concat("F");

    if (pressed) {
        cmd += "1";
    }
    else {
        cmd += "0";
    }

    cmd += funcNum;

    sendCommand(cmd);
}
