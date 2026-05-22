from openai import OpenAI
client = OpenAI()

response = client.responses.create(
  prompt={
    "id": "pmpt_6a10657544288194bd83399ed179c8ce0ff0947071024441",
    "version": "4"
  }
)