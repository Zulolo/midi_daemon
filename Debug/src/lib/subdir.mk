################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../src/lib/bluetooth.c \
../src/lib/hci.c \
../src/lib/sdp.c \
../src/lib/uuid.c 

OBJS += \
./src/lib/bluetooth.o \
./src/lib/hci.o \
./src/lib/sdp.o \
./src/lib/uuid.o 

C_DEPS += \
./src/lib/bluetooth.d \
./src/lib/hci.d \
./src/lib/sdp.d \
./src/lib/uuid.d 


# Each subdirectory must supply rules for building sources it contributes
src/lib/%.o: ../src/lib/%.c
	@echo 'Building file: $<'
	@echo 'Invoking: Cross GCC Compiler'
	arm-linux-gnueabihf-gcc -I/home/zulolo/alsa-lib-1.1.2/lib/include -I/home/zulolo/workspace -O0 -g3 -Wall -c -fmessage-length=0 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@)" -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '


