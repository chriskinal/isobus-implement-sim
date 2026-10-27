// A USB serial to I2C bridge on an ESP32, so tools/aux_input can read the I2C joystick module
// from a machine with no free I2C bus (--joystick-serial). It knows nothing of the joystick:
// one line in, one line out, at 115200 baud.
//
//   ?\n            ->  i2c-bridge 1\n
//   r <addr> <reg>\n  ->  <byte>\n, or e\n when the device does not answer (hex, no 0x)
//   s\n            ->  the addresses that answer, space separated, then \n (for wiring checks)
//
// A register read is the register number written, then one byte read after a repeated start
// (an SMBus read byte data), as Linux's i2c-dev does it. "i2c-bridge 1" is also printed at
// start, since opening the port usually resets the board. SDA is GPIO21, SCL GPIO22, 100 kHz;
// the module runs from the ESP32's 3V3.
#include <Arduino.h>
#include <Wire.h>

namespace
{
	constexpr int SDA_PIN = 21;
	constexpr int SCL_PIN = 22;
	char line[32];
	size_t length = 0;

	void handle(const char *text)
	{
		unsigned address = 0, reg = 0;
		if (0 == strcmp(text, "?"))
		{
			Serial.print("i2c-bridge 1\n");
		}
		else if (0 == strcmp(text, "s"))
		{
			for (uint8_t address = 0x08; address < 0x78; address++)
			{
				Wire.beginTransmission(address);
				if (0 == Wire.endTransmission())
				{
					Serial.printf("%02x ", address);
				}
			}
			Serial.print("\n");
		}
		else if (2 == sscanf(text, "r %x %x", &address, &reg) && address < 0x80 && reg < 0x100)
		{
			Wire.beginTransmission(static_cast<uint8_t>(address));
			Wire.write(static_cast<uint8_t>(reg));
			if (0 == Wire.endTransmission(false) && 1 == Wire.requestFrom(static_cast<uint8_t>(address), static_cast<uint8_t>(1)))
			{
				Serial.printf("%02x\n", Wire.read());
			}
			else
			{
				Serial.print("e\n");
			}
		}
		else
		{
			Serial.print("e\n");
		}
	}
}

void setup()
{
	Serial.begin(115200);
	Wire.begin(SDA_PIN, SCL_PIN, 100000);
	Serial.print("i2c-bridge 1\n");
}

void loop()
{
	while (Serial.available())
	{
		const char c = static_cast<char>(Serial.read());
		if ('\n' == c || '\r' == c)
		{
			if (length > 0)
			{
				line[length] = '\0';
				handle(line);
				length = 0;
			}
		}
		else if (length < sizeof(line) - 1)
		{
			line[length++] = c;
		}
	}
}
