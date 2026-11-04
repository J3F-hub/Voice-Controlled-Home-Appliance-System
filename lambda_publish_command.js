/*
  Lambda function: relay a voice command into AWS IoT Core (MQTT)
  ------------------------------------------------------------------
  Purpose: IFTTT cannot sign AWS requests (SigV4) or use mTLS, so it
  cannot call AWS IoT Core directly. This Lambda function sits in
  between: IFTTT calls this function's public URL (via API Gateway)
  with a plain HTTP POST, and this function uses its IAM role to
  publish the command to AWS IoT Core on your behalf.

  Runtime: Node.js 18.x or later
  Handler: index.handler

  Deploy steps (summary):
    1. AWS Console -> Lambda -> Create function -> Author from scratch
       - Name: publishSmartHomeCommand
       - Runtime: Node.js 18.x
    2. Paste this code into index.js in the Lambda console.
    3. Attach an execution role that allows "iot:Publish" on your topic
       (see the IAM policy example below this file's comments).
    4. Create an API Gateway (HTTP API) trigger for this Lambda,
       method POST, e.g. path /light-command. Copy the Invoke URL —
       this is what IFTTT's Webhooks action will call.

  Expected request body from IFTTT (JSON), e.g.:
    { "command": "light one on" }
  or in Thai:
    { "command": "ไฟดวงที่1เปิด" }

  --------------------------------------------------------------
  Example IAM policy to attach to this Lambda's execution role
  (replace REGION, ACCOUNT_ID, and the topic name if different):

  {
    "Version": "2012-10-17",
    "Statement": [
      {
        "Effect": "Allow",
        "Action": "iot:Publish",
        "Resource": "arn:aws:iot:REGION:ACCOUNT_ID:topic/smarthome/lights"
      }
    ]
  }
  --------------------------------------------------------------
*/

const { IoTDataPlaneClient, PublishCommand } = require("@aws-sdk/client-iot-data-plane");

// FILL IN: your AWS IoT Core "Device data endpoint" (same one used in the ESP32 code)
const IOT_ENDPOINT = "xxxxxxxxxxxxxx-ats.iot.ap-southeast-1.amazonaws.com";
const IOT_TOPIC = "smarthome/lights";

const client = new IoTDataPlaneClient({ endpoint: `https://${IOT_ENDPOINT}` });

exports.handler = async (event) => {
  try {
    // API Gateway (HTTP API) puts the JSON body as a string in event.body
    let body = {};
    if (event.body) {
      body = typeof event.body === "string" ? JSON.parse(event.body) : event.body;
    }

    const command = body.command;

    if (!command || typeof command !== "string") {
      return {
        statusCode: 400,
        body: JSON.stringify({ error: "Missing 'command' field in request body" }),
      };
    }

    const publishCmd = new PublishCommand({
      topic: IOT_TOPIC,
      payload: Buffer.from(command, "utf-8"),
      qos: 0,
    });

    await client.send(publishCmd);

    return {
      statusCode: 200,
      body: JSON.stringify({ status: "ok", published: command }),
    };
  } catch (err) {
    console.error("Publish failed:", err);
    return {
      statusCode: 500,
      body: JSON.stringify({ error: "Failed to publish command", details: String(err) }),
    };
  }
};
