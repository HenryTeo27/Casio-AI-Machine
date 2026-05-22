// utils/prisma.ts
import { PrismaPg } from "@prisma/adapter-pg";
import { PrismaClient } from "@prisma/client";

// Grab your connection string from env
const connectionString = process.env.POSTGRES_URL!;

// Instantiate the adapter (it will spin up and cache its own pool)
const adapter = new PrismaPg({ connectionString });

declare global {
    // Next.js might reuse the same module across invocations in dev
    // so we cache one PrismaClient globally to avoid too many connections
    var prisma: PrismaClient | undefined;
}

const prisma = globalThis.prisma ?? new PrismaClient({ adapter });

if (process.env.NODE_ENV !== "production") {
    globalThis.prisma = prisma;
}

export default prisma;
