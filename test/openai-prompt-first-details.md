from openai import OpenAI
client = OpenAI()

response = client.responses.create(
  prompt={
    "id": "pmpt_6a106f668b9c8190a9ffaac9fc0b7198025cea817cae6af5",
    "version": "2"
  }
)